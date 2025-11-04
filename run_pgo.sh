#! /usr/bin/bash
# builds and runs an optimized benchmark executable

VERBOSE=false
AGGRESSIVE_CLEANUP=false

for arg in "$@"; do
  case $arg in
    --verbose)
      VERBOSE=true
      shift
      ;;
    --aggressive-cleanup)
      AGGRESSIVE_CLEANUP=true
      shift
      ;;
  esac
done

if [ "$VERBOSE" = true ]; then
  echo "Verbose mode enabled. Build output will be shown."
fi

# terminates common background processes that might interfere with the benchmark.
terminate_background_processes() {
  echo ""
  echo "--- AGGRESSIVE CLEANUP ---"
  echo "WARNING: Attempting to kill common background processes to free up resources."
  read -n 1 -s -r -p "Press any key to continue, or Ctrl+C to abort..."
  echo ""
  
  local apps_to_kill=(
	"chrome" "firefox" "brave" "opera" "vivaldi" "edge" "safari"
	"slack" "discord" "zoom" "teams" "telegram" "signal" "whatsapp" "skype"
	"spotify" "vlc" "mpv" "plex"
	"code" "jetbrains" "sublime_text" "atom" "idea" "pycharm" "webstorm"
	"dropbox" "onedrive" "insync" "megasync"
	"docker" "virtualbox" "vmware" "qemu"
	"steam" "lutris" "heroic"
	"obsidian" "joplin" "evernote" "postman" "anydesk"
	"clangd" "nvim"
  )
  
  for app in "${apps_to_kill[@]}"; do
    # use pgrep to check if the process exists, then pkill to terminate it.
    if pgrep -f "$app" > /dev/null; then
      echo "Terminating processes matching '$app'..."
      pkill -f "$app"
    fi
  done
  echo "--------------------------"
  echo ""
}

# print CPU Info
echo "==================== CPU Information ===================="
lscpu | grep -E 'Model name:|Architecture:|CPU\(s\)|Core\(s\) per socket:|Thread\(s\) per core:'
echo "======================================================="

if [ "$AGGRESSIVE_CLEANUP" = true ]; then
  terminate_background_processes
fi

# Build and Run Benchmark
echo "Starting PGO-optimized build and benchmark..."
echo "To see build details, re-run with the --verbose flag."

# Clean and create build directory
rm -rf build
mkdir build
cd build

# helper function to run build commands verbosely or silently
run_command() {
  if [ "$VERBOSE" = true ]; then
    "$@" # show output
  else
    "$@" > /dev/null 2>&1 # redirect both stdout and stderr
  fi
}

# Stage 1: PGO Generate
run_command cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPTIM=On -DBENCHMARKS=On -DPGO_STAGE=GENERATE
run_command make -j$(nproc) global
./benchmarks/global/global > /dev/null

# Stage 2: PGO Use
run_command cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=On -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPTIM=On -DBENCHMARKS=On -DPGO_STAGE=USE
run_command make -j$(nproc) global

# Stage 3: Benchmark
echo ""
echo "--- Benchmark Results ---"
echo "Pinning benchmark to specific CPU cores for maximum consistency."
CORES=$(($(nproc)-1))

if [ "$VERBOSE" = true ]; then
  echo "Running single-threaded on core 0..."
  taskset -c 0 ./benchmarks/global/global
  echo "Running multi-threaded on cores 0-$CORES..."
  taskset -c 0-$CORES ./benchmarks/global/global --multi
else
  taskset -c 0 ./benchmarks/global/global 2> /dev/null
  taskset -c 0-$CORES ./benchmarks/global/global --multi 2> /dev/null
fi
echo "-------------------------"
echo ""
cd ..
echo "Benchmark complete."

