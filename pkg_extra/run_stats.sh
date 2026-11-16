#!/usr/bin/env bash
# Тестовий запуск зі збором статистики. Після закриття програми поруч з p2pnode
# з'явиться mkr_stats_*.zip - надішліть його. Використання: ./run_stats.sh [ім'я] [UDP-порт]
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Без імені: профіль отримує автоматичне ім'я (змінити можна в програмі: Edit profile).
if [ -n "${1:-}" ]; then
  ./p2pnode --stats --alias "$1" --port "${2:-47000}" --key "$PWD/linux.key"
else
  ./p2pnode --stats --port "${2:-47000}" --key "$PWD/linux.key"
fi
echo; echo "Надішліть файл:"; ls -1 mkr_stats_*.zip
