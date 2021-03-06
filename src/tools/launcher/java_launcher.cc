// Copyright 2017 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sstream>
#include <string>
#include <vector>

#include "src/tools/launcher/java_launcher.h"
#include "src/tools/launcher/util/launcher_util.h"

namespace bazel {
namespace launcher {

using std::getline;
using std::ofstream;
using std::ostringstream;
using std::string;
using std::stringstream;
using std::vector;

// The runfile path of java binary, eg. local_jdk/bin/java.exe
static constexpr const char* JAVA_BIN_PATH = "java_bin_path";
static constexpr const char* JAR_BIN_PATH = "jar_bin_path";
static constexpr const char* CLASSPATH = "classpath";
static constexpr const char* JAVA_START_CLASS = "java_start_class";
static constexpr const char* JVM_FLAGS = "jvm_flags";

// Check if a string start with a certain prefix.
// If it's true, store the substring without the prefix in value.
// If value is quoted, then remove the quotes.
static bool GetFlagValue(const string& str, const string& prefix,
                         string* value_ptr) {
  if (str.compare(0, prefix.length(), prefix)) {
    return false;
  }
  string& value = *value_ptr;
  value = str.substr(prefix.length());
  int len = value.length();
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
    value = value.substr(1, len - 2);
  }
  return true;
}

// Parses one launcher flag and updates this object's state accordingly.
//
// Returns true if the flag is a valid launcher flag; false otherwise.
bool JavaBinaryLauncher::ProcessWrapperArgument(const string& argument) {
  string flag_value;
  if (argument.compare("--debug") == 0) {
    string default_jvm_debug_port;
    if (GetEnv("DEFAULT_JVM_DEBUG_PORT", &default_jvm_debug_port)) {
      this->jvm_debug_port = default_jvm_debug_port;
    } else {
      this->jvm_debug_port = "5005";
    }
  } else if (GetFlagValue(argument, "--debug=", &flag_value)) {
    this->jvm_debug_port = flag_value;
  } else if (GetFlagValue(argument, "--main_advice=", &flag_value)) {
    this->main_advice = flag_value;
  } else if (GetFlagValue(argument, "--main_advice_classpath=", &flag_value)) {
    this->main_advice_classpath = flag_value;
  } else if (GetFlagValue(argument, "--jvm_flag=", &flag_value)) {
    this->jvm_flags_cmdline.push_back(flag_value);
  } else if (GetFlagValue(argument, "--jvm_flags=", &flag_value)) {
    stringstream flag_value_ss(flag_value);
    string item;
    while (getline(flag_value_ss, item, ' ')) {
      this->jvm_flags_cmdline.push_back(item);
    }
  } else if (argument.compare("--singlejar") == 0) {
    this->singlejar = true;
  } else if (argument.compare("--print_javabin") == 0) {
    this->print_javabin = true;
  } else if (GetFlagValue(argument, "--classpath_limit=", &flag_value)) {
    this->classpath_limit = std::stoi(flag_value);
  } else {
    return false;
  }
  return true;
}

vector<string> JavaBinaryLauncher::ProcessesCommandLine() {
  vector<string> args;
  bool first = 1;
  for (const auto& arg : this->GetCommandlineArguments()) {
    // Skip the first arugment.
    if (first) {
      first = 0;
      continue;
    }
    string flag_value;
    // TODO(pcloudy): Should rename this flag to --native_launcher_flag.
    // But keep it as it is for now to be consistent with the shell script
    // launcher.
    if (GetFlagValue(arg, "--wrapper_script_flag=", &flag_value)) {
      if (!ProcessWrapperArgument(flag_value)) {
        die("invalid wrapper argument '%s'", arg);
      }
    } else if (!args.empty() || !ProcessWrapperArgument(arg)) {
      args.push_back(arg);
    }
  }
  return args;
}

string JavaBinaryLauncher::CreateClasspathJar(const string& classpath) {
  static constexpr const char* URI_PREFIX = "file:/";
  ostringstream manifest_classpath;
  manifest_classpath << "Class-Path:";
  stringstream classpath_ss(classpath);
  string path;
  while (getline(classpath_ss, path, ';')) {
    manifest_classpath << ' ';
    manifest_classpath << URI_PREFIX;
    for (const auto& x : path) {
      if (x == ' ') {
        manifest_classpath << "%20";
      } else {
        manifest_classpath << x;
      }
    }
  }

  string binary_base_path =
      GetBinaryPathWithoutExtension(this->GetCommandlineArguments()[0]);
  string jar_manifest_file_path = binary_base_path + ".jar_manifest";
  ofstream jar_manifest_file(jar_manifest_file_path);
  jar_manifest_file << "Manifest-Version: 1.0\n";
  // No line in the MANIFEST.MF file may be longer than 72 bytes.
  // A space prefix indicates the line is still the content of the last
  // attribute.
  string manifest_classpath_str = manifest_classpath.str();
  for (size_t i = 0; i < manifest_classpath_str.length(); i += 71) {
    if (i > 0) {
      jar_manifest_file << " ";
    }
    jar_manifest_file << manifest_classpath_str.substr(i, 71) << "\n";
  }
  jar_manifest_file.close();

  // Create the command for generating classpath jar.
  // We pass the command to cmd.exe to use redirection for suppressing output.
  string manifest_jar_path = binary_base_path + "-classpath.jar";
  string jar_bin = this->Rlocation(this->GetLaunchInfoByKey(JAR_BIN_PATH));
  vector<string> arguments;
  arguments.push_back("/c");

  ostringstream jar_command;
  jar_command << jar_bin << " cvfm ";
  jar_command << manifest_jar_path << " ";
  jar_command << jar_manifest_file_path << " ";
  jar_command << "> nul";
  arguments.push_back(jar_command.str());

  if (this->LaunchProcess("cmd.exe", arguments) != 0) {
    die("Couldn't create classpath jar: %s", manifest_jar_path.c_str());
  }

  return manifest_jar_path;
}

ExitCode JavaBinaryLauncher::Launch() {
  // Parse the original command line.
  vector<string> remaining_args = this->ProcessesCommandLine();

  // Set JAVA_RUNFILES
  string java_runfiles;
  if (!GetEnv("JAVA_RUNFILES", &java_runfiles)) {
    java_runfiles = this->GetRunfilesPath();
  }
  SetEnv("JAVA_RUNFILES", java_runfiles);

  // Print Java binary path if needed
  string java_bin = this->Rlocation(this->GetLaunchInfoByKey(JAVA_BIN_PATH),
                                    /*need_workspace_name =*/false);
  if (this->print_javabin ||
      this->GetLaunchInfoByKey(JAVA_START_CLASS) == "--print_javabin") {
    printf("%s\n", java_bin.c_str());
    return 0;
  }

  ostringstream classpath;

  // Run deploy jar if needed, otherwise generate the CLASSPATH by rlocation.
  if (this->singlejar) {
    string deploy_jar =
        GetBinaryPathWithoutExtension(this->GetCommandlineArguments()[0]) +
        "_deploy.jar";
    if (!DoesFilePathExist(deploy_jar.c_str())) {
      die("Option --singlejar was passed, but %s does not exist.\n  (You may "
          "need to build it explicitly.)",
          deploy_jar.c_str());
    }
    classpath << deploy_jar << ';';
  } else {
    // Add main advice classpath if exists
    if (!this->main_advice_classpath.empty()) {
      classpath << this->main_advice_classpath << ';';
    }
    string path;
    stringstream classpath_ss(this->GetLaunchInfoByKey(CLASSPATH));
    while (getline(classpath_ss, path, ';')) {
      classpath << this->Rlocation(path) << ';';
    }
  }

  // Set jvm debug options
  ostringstream jvm_debug_flags;
  if (!this->jvm_debug_port.empty()) {
    string jvm_debug_suspend;
    if (!GetEnv("DEFAULT_JVM_DEBUG_SUSPEND", &jvm_debug_suspend)) {
      jvm_debug_suspend = "y";
    }
    jvm_debug_flags << "-agentlib:jdwp=transport=dt_socket,server=y";
    jvm_debug_flags << ",suspend=" << jvm_debug_suspend;
    jvm_debug_flags << ",address=" << jvm_debug_port;

    string value;
    if (GetEnv("PERSISTENT_TEST_RUNNER", &value) && value == "true") {
      jvm_debug_flags << ",quiet=y";
    }
  }

  // Get jvm flags from JVM_FLAGS environment variable and JVM_FLAGS launch info
  vector<string> jvm_flags;
  string jvm_flags_env;
  GetEnv("JVM_FLAGS", &jvm_flags_env);
  string flag;
  stringstream jvm_flags_env_ss(jvm_flags_env);
  while (getline(jvm_flags_env_ss, flag, ' ')) {
    jvm_flags.push_back(flag);
  }
  stringstream jvm_flags_launch_info_ss(this->GetLaunchInfoByKey(JVM_FLAGS));
  while (getline(jvm_flags_launch_info_ss, flag, ' ')) {
    jvm_flags.push_back(flag);
  }

  // Check if TEST_TMPDIR is available to use for scratch.
  string test_tmpdir;
  if (GetEnv("TEST_TMPDIR", &test_tmpdir) &&
      DoesDirectoryPathExist(test_tmpdir.c_str())) {
    jvm_flags.push_back("-Djava.io.tmpdir=" + test_tmpdir);
  }

  // Construct the final command line arguments
  vector<string> arguments;
  // Add classpath flags
  arguments.push_back("-classpath");
  // Check if CLASSPATH is over classpath length limit.
  // If it does, then we create a classpath jar to pass CLASSPATH value.
  string classpath_str = classpath.str();
  if (classpath_str.length() > this->classpath_limit) {
    arguments.push_back(CreateClasspathJar(classpath_str));
  } else {
    arguments.push_back(classpath_str);
  }
  // Add JVM debug flags
  string jvm_debug_flags_str = jvm_debug_flags.str();
  if (!jvm_debug_flags_str.empty()) {
    arguments.push_back(jvm_debug_flags_str);
  }
  // Add JVM flags parsed from env and launch info.
  for (const auto& arg : jvm_flags) {
    arguments.push_back(arg);
  }
  // Add JVM flags parsed from command line.
  for (const auto& arg : this->jvm_flags_cmdline) {
    arguments.push_back(arg);
  }
  // Add main advice class
  if (!this->main_advice.empty()) {
    arguments.push_back(this->main_advice);
  }
  // Add java start class
  arguments.push_back(this->GetLaunchInfoByKey(JAVA_START_CLASS));
  // Add the remaininng arguements, they will be passed to the program.
  for (const auto& arg : remaining_args) {
    arguments.push_back(arg);
  }

  return this->LaunchProcess(java_bin, arguments);
}

}  // namespace launcher
}  // namespace bazel
