@echo off
REM RACE kernel HLS flow on Windows: csim -> synth -> cosim.
REM First load Vitis, e.g.  call C:\AMDDesignTools\2026.1\Vitis\settings64.bat
setlocal
cd /d "%~dp0"
set WORK=build_risk
where vitis-run >nul 2>&1 || (echo error: vitis-run not on PATH - load Vitis settings64.bat & exit /b 2)
echo == C simulation ==
call vitis-run --mode hls --csim --config risk_hls.cfg --work_dir %WORK% || exit /b 1
echo == C synthesis ==
call v++ -c --mode hls --config risk_hls.cfg --work_dir %WORK% || exit /b 1
echo == C/RTL co-simulation ==
call vitis-run --mode hls --cosim --config risk_hls.cfg --work_dir %WORK% || exit /b 1
if not exist ..\reports mkdir ..\reports
for /r "%WORK%" %%f in (*_csynth.rpt *_cosim.rpt) do copy /y "%%f" ..\reports\ >nul
echo RISK HLS FLOW: PASS  (reports copied to accelerator\reports)
