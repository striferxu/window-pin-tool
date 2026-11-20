@echo off
chcp 65001 >nul
echo 正在刷新 Windows 图标缓存...
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 2 /nobreak >nul
del /a /f /q "%LocalAppData%\IconCache.db" >nul 2>&1
del /a /f /q "%LocalAppData%\Microsoft\Windows\Explorer\iconcache_*.db" >nul 2>&1
del /a /f /q "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
start explorer.exe
echo.
echo 图标缓存已刷新，桌面与资源管理器会重新加载。
echo 如果 WindowPin.exe 仍显示旧图标，请把它移动到另一个文件夹再看。
pause
