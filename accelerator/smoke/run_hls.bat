@echo off
REM Gate 1 HLS smoke flow on Windows: csim -> synth -> cosim.
REM Run from a shell where Vitis 2026.1 settings are loaded, e.g.
REM   call C:\AMDDesignTools\2026.1\Vitis\settings64.bat
setlocal
cd /d "%~dp0"
set WORK=build_smoke

where vitis-run >nul 2>&1 || (echo error: vitis-run not on PATH - load Vitis settings64.bat & exit /b 2)

echo == C simulation ==
call vitis-run --mode hls --csim --config hls_config.cfg --work_dir %WORK% || exit /b 1
echo == C synthesis ==
call v++ -c --mode hls --config hls_config.cfg --work_dir %WORK% || exit /b 1
echo == C/RTL co-simulation ==
call vitis-run --mode hls --cosim --config hls_config.cfg --work_dir %WORK% || exit /b 1

if not exist reports mkdir reports
for /r "%WORK%" %%f in (*_csynth.rpt *_cosim.rpt) do copy /y "%%f" reports\ >nul
echo GATE1 HLS FLOW: PASS  (reports copied to accelerator\smoke\reports)
