#!/bin/sh
# 🎙️ Local Voice Command Listener for Spotify Soloist on Yandex Station
# Catches native hardware phrase spotter logs ("дальше", "назад", "пауза", "играй", "хватит")
# Works 100% offline directly on the speaker without any PC!

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_SH="$SCRIPT_DIR/service.sh"
MAIND_LOG="/data/quasar/daemons_logs/maind.log"
FIFO="/tmp/soloist_voice.fifo"

ctl() {
    "$SERVICE_SH" ctl "$@"
}

action_for() {
    case "$1" in
        дальше|следующую|следующая|следующий|вперед) echo "next" ;;
        назад|предыдущий|предыдущая) echo "prev" ;;
        пауза|стоп|хватит) echo "pause" ;;
        продолжи|играй|пуск) echo "play" ;;
        *) echo "" ;;
    esac
}

run() {
    if [ ! -f "$MAIND_LOG" ]; then
        echo "ℹ️ [Voice] $MAIND_LOG не найден (Quasar не запущен). Локальный споттер спит."
        exit 0
    fi

    echo "🎙️ [Voice] Запуск локального слушателя команд (дальше/назад/пауза/играй)..."
    rm -f "$FIFO"
    mkfifo "$FIFO"
    tail -n 0 -F "$MAIND_LOG" 2>/dev/null > "$FIFO" &
    TAIL_PID=$!
    trap 'kill $TAIL_PID 2>/dev/null || true; rm -f "$FIFO"; exit 0' TERM INT EXIT

    last_action=""
    last_time=0
    stop_count=0
    stop_first=0

    while IFS= read -r line; do
        case "$line" in
            *'Command phrase spotted: '*)
                phrase=${line##*Command phrase spotted: }
                ;;
            *'Phrase '*' is disabled for current states'*)
                phrase=${line##*Phrase }
                ;;
            *) continue ;;
        esac
        phrase=${phrase%% *}

        # "хватит" 3 раза за 6 секунд -> перезагрузка
        if [ "$phrase" = "хватит" ]; then
            now=$(date +%s)
            if [ $((now - stop_first)) -gt 6 ]; then
                stop_first=$now
                stop_count=0
            fi
            stop_count=$((stop_count + 1))
            if [ "$stop_count" -ge 3 ]; then
                echo "🛑 [Voice] 'хватит' x3 -> перезагрузка колонки..."
                sync
                reboot
            fi
        fi

        action=$(action_for "$phrase")
        [ -n "$action" ] || continue

        now=$(date +%s)
        if [ "$action" = "$last_action" ] && [ $((now - last_time)) -lt 2 ]; then
            continue
        fi
        last_action=$action
        last_time=$now

        echo "🗣️ [Voice] Команда: '$phrase' -> ctl $action"
        ctl "$action" >/dev/null 2>&1 || true
    done < "$FIFO"
}

case "${1:-run}" in
    run) run ;;
    *) echo "Usage: $0 run" >&2; exit 2 ;;
esac
