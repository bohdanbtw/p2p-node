@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Тестовий запуск зі збором статистики (--stats).
echo Після закриття програми поруч з p2pnode.exe з'явиться файл mkr_stats_*.zip - надішліть його.
echo.
p2pnode.exe --stats --port 47000 --key "%~dp0win.key" %*
echo.
echo Готово. Надішліть файл mkr_stats_*.zip з цієї папки:
dir /b mkr_stats_*.zip
pause
