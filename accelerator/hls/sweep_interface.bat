@echo off
REM Tries the bus-packing variants of the RACE kernel (synthesis only, ~1 min each) and prints the achieved II.
REM First load Vitis:  call D:\Xilinx\2025.1\Vitis\settings64.bat
setlocal
cd /d "%~dp0"
where v++ >nul 2>&1 || (echo error: v++ not on PATH - load Vitis settings64.bat & exit /b 2)
for %%V in (0 1 2 3) do (
  echo == variant %%V
  powershell -NoProfile -Command "(Get-Content risk_hls.cfg) -replace '^syn.cflags=.*','syn.cflags=-I../include -DRISK_HLS_VARIANT=%%V' | Set-Content sweep_%%V.cfg"
  if exist build_sweep_%%V rmdir /s /q build_sweep_%%V
  call v++ -c --mode hls --config sweep_%%V.cfg --work_dir build_sweep_%%V > sweep_%%V.log 2>&1
  findstr /c:"Final II" /c:"ERROR" sweep_%%V.log | findstr /i "risk_loop ERROR"
)
echo == done. Pick the variant with Final II = 1, then set it as the default in risk_kernel.cpp
