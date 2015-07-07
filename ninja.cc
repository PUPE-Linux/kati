// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

#include "ninja.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <unordered_set>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "flags.h"
#include "log.h"
#include "string_piece.h"
#include "stringprintf.h"
#include "strutil.h"
#include "var.h"

static StringPiece FindCommandLineFlagWithArg(StringPiece cmd,
                                              StringPiece name) {
  size_t index = cmd.find(name);
  if (index == string::npos)
    return StringPiece();

  StringPiece val = TrimLeftSpace(cmd.substr(index + name.size()));
  index = val.find(name);
  while (index != string::npos) {
    val = TrimLeftSpace(val.substr(index + name.size()));
    index = val.find(name);
  }

  index = val.find(' ');
  CHECK(index != string::npos);
  return val.substr(0, index);
}

static bool StripPrefix(StringPiece p, StringPiece* s) {
  if (!HasPrefix(*s, p))
    return false;
  *s = s->substr(p.size());
  return true;
}

static bool IsAndroidCompileCommand(StringPiece cmd) {
  if (!StripPrefix("prebuilts/", &cmd))
    return false;
  if (!StripPrefix("gcc/", &cmd) && !StripPrefix("clang/", &cmd))
    return false;

  size_t index = cmd.find(' ');
  if (index == string::npos)
    return false;
  StringPiece cc = cmd.substr(0, index);
  if (!HasSuffix(cc, "gcc") && !HasSuffix(cc, "g++") &&
      !HasSuffix(cc, "clang") && !HasSuffix(cc, "clang++")) {
    return false;
  }

  cmd = cmd.substr(index);
  return cmd.find(" -c ") != string::npos;
}

static bool GetDepfileFromCommandImpl(StringPiece cmd, string* out) {
  if (cmd.find(StringPiece(" -MD ")) == string::npos &&
      cmd.find(StringPiece(" -MMD ")) == string::npos) {
    return false;
  }

  StringPiece mf = FindCommandLineFlagWithArg(cmd, StringPiece(" -MF "));
  if (!mf.empty()) {
    mf.AppendToString(out);
    return true;
  }

  StringPiece o = FindCommandLineFlagWithArg(cmd, StringPiece(" -o "));
  if (o.empty()) {
    ERROR("Cannot find the depfile in %s", cmd.as_string().c_str());
    return false;
  }

  StripExt(o).AppendToString(out);
  *out += ".d";
  return true;
}

bool GetDepfileFromCommand(StringPiece cmd, string* out) {
  CHECK(cmd.get(cmd.size()-1) == ' ');

  if (!GetDepfileFromCommandImpl(cmd, out))
    return false;

  // A hack for Android - llvm-rs-cc seems not to emit a dep file.
  if (cmd.find("bin/llvm-rs-cc ") != string::npos) {
    return false;
  }

  // TODO: A hack for Makefiles generated by automake.

  // A hack for Android to get .P files instead of .d.
  string p;
  StripExt(*out).AppendToString(&p);
  p += ".P";
  if (cmd.find(p) != string::npos) {
    *out = p;
    return true;
  }

  // A hack for Android. For .s files, GCC does not use C
  // preprocessor, so it ignores -MF flag.
  string as = "/";
  StripExt(Basename(*out)).AppendToString(&as);
  as += ".s";
  if (cmd.find(as) != string::npos) {
    return false;
  }

  return true;
}

class NinjaGenerator {
 public:
  NinjaGenerator(const char* ninja_suffix, Evaluator* ev)
      : ce_(ev), ev_(ev), fp_(NULL), rule_id_(0) {
    ev_->set_avoid_io(true);
    if (g_goma_dir)
      gomacc_ = StringPrintf("%s/gomacc ", g_goma_dir);
    if (ninja_suffix) {
      ninja_suffix_ = ninja_suffix;
    }
  }

  ~NinjaGenerator() {
    ev_->set_avoid_io(false);
  }

  void Generate(const vector<DepNode*>& nodes) {
    GenerateShell();
    GenerateNinja(nodes);
  }

 private:
  string GenRuleName() {
    return StringPrintf("rule%d", rule_id_++);
  }

  StringPiece TranslateCommand(const char* in) {
    const size_t orig_size = cmd_buf_.size();
    bool prev_backslash = false;
    char quote = 0;
    bool done = false;
    for (; *in && !done; in++) {
      switch (*in) {
        case '#':
          if (quote == 0 && !prev_backslash) {
            done = true;
            break;
          }

        case '\'':
        case '"':
        case '`':
          if (quote) {
            if (quote == *in)
              quote = 0;
          } else if (!prev_backslash) {
            quote = *in;
          }
          cmd_buf_ += *in;
          break;

        case '$':
          cmd_buf_ += "$$";
          break;

        case '\t':
          cmd_buf_ += ' ';
          break;

        case '\n':
          if (prev_backslash) {
            cmd_buf_[cmd_buf_.size()-1] = ' ';
          } else {
            cmd_buf_ += ' ';
          }
          break;

        case '\\':
          prev_backslash = !prev_backslash;
          cmd_buf_ += '\\';
          break;

        default:
          cmd_buf_ += *in;
          prev_backslash = false;
      }
    }

    while (true) {
      char c = cmd_buf_[cmd_buf_.size()-1];
      if (!isspace(c) && c != ';')
        break;
      cmd_buf_.resize(cmd_buf_.size() - 1);
    }

    return StringPiece(cmd_buf_.data() + orig_size,
                       cmd_buf_.size() - orig_size);
  }

  bool GenShellScript(const vector<Command*>& commands) {
    bool use_gomacc = false;
    bool should_ignore_error = false;
    cmd_buf_.clear();
    for (const Command* c : commands) {
      if (!cmd_buf_.empty()) {
        if (should_ignore_error) {
          cmd_buf_ += " ; ";
        } else {
          cmd_buf_ += " && ";
        }
      }
      should_ignore_error = c->ignore_error;

      const char* in = c->cmd->c_str();
      while (isspace(*in))
        in++;

      bool needs_subshell = commands.size() > 1;
      if (*in == '(') {
        needs_subshell = false;
      }

      if (needs_subshell)
        cmd_buf_ += '(';

      size_t cmd_start = cmd_buf_.size();
      StringPiece translated = TranslateCommand(in);
      if (translated.empty()) {
        cmd_buf_ += "true";
      } else if (g_goma_dir && IsAndroidCompileCommand(translated)) {
        cmd_buf_.insert(cmd_start, gomacc_);
        use_gomacc = true;
      }

      if (c == commands.back() && c->ignore_error) {
        cmd_buf_ += " ; true";
      }

      if (needs_subshell)
        cmd_buf_ += ')';
    }
    return g_goma_dir && !use_gomacc;
  }

  void EmitDepfile() {
    cmd_buf_ += ' ';
    string depfile;
    bool result = GetDepfileFromCommand(cmd_buf_, &depfile);
    cmd_buf_.resize(cmd_buf_.size()-1);
    if (!result)
      return;
    fprintf(fp_, " depfile = %s\n", depfile.c_str());
  }

  void EmitNode(DepNode* node) {
    auto p = done_.insert(node->output);
    if (!p.second)
      return;

    if (node->cmds.empty() &&
        node->deps.empty() && node->order_onlys.empty() && !node->is_phony) {
      return;
    }

    vector<Command*> commands;
    ce_.Eval(node, &commands);

    string rule_name = "phony";
    bool use_local_pool = false;
    if (!commands.empty()) {
      rule_name = GenRuleName();
      fprintf(fp_, "rule %s\n", rule_name.c_str());
      fprintf(fp_, " description = build $out\n");

      use_local_pool |= GenShellScript(commands);
      EmitDepfile();

      // It seems Linux is OK with ~130kB.
      // TODO: Find this number automatically.
      if (cmd_buf_.size() > 100 * 1000) {
        fprintf(fp_, " rspfile = $out.rsp\n");
        fprintf(fp_, " rspfile_content = %s\n", cmd_buf_.c_str());
        fprintf(fp_, " command = sh $out.rsp\n");
      } else {
        fprintf(fp_, " command = %s\n", cmd_buf_.c_str());
      }
    }

    EmitBuild(node, rule_name);
    if (use_local_pool)
      fprintf(fp_, " pool = local_pool\n");

    for (DepNode* d : node->deps) {
      EmitNode(d);
    }
    for (DepNode* d : node->order_onlys) {
      EmitNode(d);
    }
  }

  void EmitBuild(DepNode* node, const string& rule_name) {
    fprintf(fp_, "build %s: %s", node->output.c_str(), rule_name.c_str());
    vector<Symbol> order_onlys;
    for (DepNode* d : node->deps) {
      fprintf(fp_, " %s", d->output.c_str());
    }
    if (!node->order_onlys.empty()) {
      fprintf(fp_, " ||");
      for (DepNode* d : node->order_onlys) {
        fprintf(fp_, " %s", d->output.c_str());
      }
    }
    fprintf(fp_, "\n");
  }

  string GetNinjaFilename() const {
    return StringPrintf("build%s.ninja", ninja_suffix_.c_str());
  }

  string GetShellScriptFilename() const {
    return StringPrintf("ninja%s.sh", ninja_suffix_.c_str());
  }

  void GenerateNinja(const vector<DepNode*>& nodes) {
    fp_ = fopen(GetNinjaFilename().c_str(), "wb");
    if (fp_ == NULL)
      PERROR("fopen(build.ninja) failed");

    fprintf(fp_, "# Generated by kati\n");
    fprintf(fp_, "\n");

    if (g_goma_dir) {
      fprintf(fp_, "pool local_pool\n");
      fprintf(fp_, " depth = %d\n", g_num_jobs);
    }

    for (DepNode* node : nodes) {
      EmitNode(node);
    }

    fclose(fp_);
  }

  void GenerateShell() {
    FILE* fp = fopen(GetShellScriptFilename().c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(ninja.sh) failed");

    shared_ptr<string> shell = ev_->EvalVar(kShellSym);
    if (shell->empty())
      shell = make_shared<string>("/bin/sh");
    fprintf(fp, "#!%s\n", shell->c_str());

    for (const auto& p : ev_->exports()) {
      if (p.second) {
        shared_ptr<string> val = ev_->EvalVar(p.first);
        fprintf(fp, "export %s=%s\n", p.first.c_str(), val->c_str());
      } else {
        fprintf(fp, "unset %s\n", p.first.c_str());
      }
    }

    fprintf(fp, "exec ninja -f %s ", GetNinjaFilename().c_str());
    if (g_goma_dir) {
      fprintf(fp, "-j300 ");
    }
    fprintf(fp, "\"$@\"\n");

    if (chmod(GetShellScriptFilename().c_str(), 0755) != 0)
      PERROR("chmod ninja.sh failed");
  }

  CommandEvaluator ce_;
  Evaluator* ev_;
  FILE* fp_;
  unordered_set<Symbol> done_;
  int rule_id_;
  string cmd_buf_;
  string gomacc_;
  string ninja_suffix_;
};

void GenerateNinja(const char* ninja_suffix,
                   const vector<DepNode*>& nodes, Evaluator* ev) {
  NinjaGenerator ng(ninja_suffix, ev);
  ng.Generate(nodes);
}