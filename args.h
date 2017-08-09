// gpuOwL, a GPU OpenCL Lucas-Lehmer primality checker.
// Copyright (C) 2017 Mihai Preda.

#include "clwrap.h"
#include "common.h"

#include <string>
#include <cstring>

struct Args {
  static constexpr int DEFAULT_LOGSTEP = 20000;
  std::string clArgs, uid;
  int logStep, saveStep, checkStep;
  int device;
  bool timeKernels, selfTest, useLegacy, safe;
  
  Args() {
    clArgs = "";
    logStep   = DEFAULT_LOGSTEP;
    saveStep  = 0;
    checkStep = 0;
    device    = -1;
    
    timeKernels = false;
    selfTest    = false;
    useLegacy   = false;
    safe        = false;
  }

  void logConfig() {
    std::string uidStr    = (uid.empty()    ? "" : " -uid "    + uid);
    std::string clStr     = (clArgs.empty() ? "" : " -cl \""   + clArgs + "\"");
    
    std::string tailStr =
      uidStr
      + (safe ? " -supersafe" : "")
      + clStr
      + (selfTest    ? " -selftest"     : "")
      + (timeKernels ? " -time kernels" : "")
      + (useLegacy   ? " -legacy"       : "");
      
    log("Config: -logstep %d -savestep %d -checkstep %d%s\n", logStep, saveStep, checkStep, tailStr.c_str());
  }

  // return false to stop.
  bool parse(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      log("Command line options:\n"
          "-logstep  <N>     : to log every <N> iterations (default %d)\n"
          "-savestep <N>     : to persist checkpoint every <N> iterations (default 500*logstep == %d)\n"
          "-checkstep <N>    : do Jacobi-symbol check every <N> iterations (default 10*logstep == %d)\n"
          "-uid user/machine : set UID: string to be prepended to the result line\n"
          "-supersafe        : use iterative double-check for reliable results on unreliable hardware\n"
          "-cl \"<OpenCL compiler options>\", e.g. -cl \"-save-temps=tmp/ -O2\"\n"
          "-selftest         : perform self tests from 'selftest.txt'\n"
          "                    Self-test mode does not load/save checkpoints, worktodo.txt or results.txt.\n"
          "-time kernels     : to benchmark kernels (logstep must be > 1)\n"
          "-legacy           : use legacy kernels\n\n"
          "-device <N>       : select specific device among:\n",
          logStep, 500 * logStep, 10 * logStep);
      
      cl_device_id devices[16];
      int ndev = getDeviceIDs(false, 16, devices);
      for (int i = 0; i < ndev; ++i) {
        char info[256];
        getDeviceInfo(devices[i], sizeof(info), info);
        log("    %d : %s\n", i, info);
      }

      log("\nFiles used by gpuOwL:\n"
          "    - worktodo.txt : contains exponents to test \"Test=N\", one per line\n"
          "    - results.txt : contains LL results\n"
          "    - cN.ll : the most recent checkpoint for exponent <N>; will resume from here\n"
          "    - tN.ll : the previous checkpoint, to be used if cN.ll is lost or corrupted\n"
          "    - bN.ll : a temporary checkpoint that is renamed to cN.ll once successfully written\n"
          "    - sN.iteration.residue.ll : a persistent checkpoint at the given iteration\n");

      log("\nThe lines in worktodo.txt must be of one of these forms:\n"
          "Test=70100200\n"
          "Test=3181F68030F6BF3DCD32B77337D5EF6B,70100200,75,1\n"
          "DoubleCheck=3181F68030F6BF3DCD32B77337D5EF6B,70100200,75,1\n"
          "Test=0,70100200,0,0\n");
      
      return false;
    } else if (!strcmp(arg, "-logstep")) {
      if (i < argc - 1) {
        logStep = atoi(argv[++i]);
        if (logStep <= 0) {
          log("invalid -logstep '%s'\n", argv[i]);
          return false;
        }
      } else {
        log("-logstep expects <N> argument\n");
        return false;
      }
    } else if (!strcmp(arg, "-savestep")) {
      if (i < argc - 1) {
        saveStep = atoi(argv[++i]);
        if (saveStep <= 0) {
          log("invalid -savestep '%s'\n", argv[i]);
          return false;
        }
      } else {
        log("-savestep expects <N> argument\n");
        return false;
      }
    } else if (!strcmp(arg, "-checkstep")) {
      if (i < argc - 1) {
        checkStep = atoi(argv[++i]);
        if (checkStep <= 0) {
          log("invalid -checkstep '%s'\n", argv[i]);
          return false;
        }
      } else {
        log("-checkstep expects <N> argument\n");
        return false;
      }
    } else if (!strcmp(arg, "-uid")) {
      if (i < argc - 1) {
        uid = argv[++i];
      } else {
        log("-uid expects userName/computerName\n");
        return false;
      }
    } else if (!strcmp(arg, "-supersafe")) {
      safe = true;
    } else if (!strcmp(arg, "-cl")) {
      if (i < argc - 1) {
        clArgs = argv[++i];
      } else {
        log("-cl expects options string to pass to CL compiler\n");
        return false;
      }
    } else if (!strcmp(arg, "-selftest")) {
      selfTest = true;
    } else if(!strcmp(arg, "-time")) {
      if (i < argc - 1 && !strcmp(argv[++i], "kernels")) {
        timeKernels = true;
      } else {
        log("-time expects 'kernels'\n");
        return false;
      }
    } else if (!strcmp(arg, "-legacy")) {
      useLegacy = true;
    } else if (!strcmp(arg, "-device")) {
      if (i < argc - 1) {
        device = atoi(argv[++i]);
        int nDevices = getNumberOfDevices();
        if (device < 0 || device >= nDevices) {
          log("invalid -device %d (must be between [0, %d]\n", device, nDevices - 1);
          return false;
        }        
      } else {
        log("-device expects <N> argument\n");
        return false;
      }
    } else {
      log("Argument '%s' not understood\n", arg);
      return false;
    }
  }

  assert(logStep > 0);
  if (!saveStep)  { saveStep  = logStep * 500; }
  if (!checkStep) { checkStep = logStep * 10;  }
  
  if (saveStep < logStep)  { saveStep = logStep; }
  if (checkStep < logStep) { checkStep = logStep; }

  // make them multiple of logStep
  saveStep  -= saveStep % logStep;
  checkStep -= checkStep % logStep;

  if (timeKernels && logStep == 1) {
    log("Ignoring time kernels because logStep == 1\n");
    timeKernels = false;
  }

  logConfig();
  return true;
}

};
