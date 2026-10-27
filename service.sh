#!/bin/sh
# 🎵 Spotify Soloist Service for Yandex Station Mini 2
# Lightweight, self-contained Spotify Connect daemon (no bloated PulseAudio daemon required)

set -e

INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$INSTALL_DIR/bin/soloist"
GLIBC="$INSTALL_DIR/glibc"
LIB="$INSTALL_DIR/lib"
INTERP="$GLIBC/ld-linux-armhf.so.3"

DATA_DIR="${SOLOIST_DATA_DIR:-/data/soloist_data}"
CACHE_DIR="${SOLOIST_CACHE_DIR:-/tmp/soloist_cache}"
LOG_DIR="${SOLOIST_LOG_DIR:-/tmp/soloist_logs}"
PID_FILE="$LOG_DIR/soloist.pid"
LOG_FILE="$LOG_DIR/soloist.log"
PULSE_LOG="/tmp/pulse_debug.log"

NAME_FILE="$INSTALL_DIR/device_name.txt"
KEY_FILE="$INSTALL_DIR/api_key.txt"

DEVICE_NAME="Yandex Mini 2"
[ ! -s "$NAME_FILE" ] || DEVICE_NAME="$(cat "$NAME_FILE" | tr -d '\r\n')"

get_api_key() {
    if [ -n "${SOLOIST_API_KEY:-}" ]; then
        echo "$SOLOIST_API_KEY"
    elif [ -s "$KEY_FILE" ]; then
        cat "$KEY_FILE" | tr -d '\r\n '
    elif [ -s /data/soloist_api_key.txt ]; then
        cat /data/soloist_api_key.txt | tr -d '\r\n '
    fi
}

setup_audio_hardware() {
    # 0. Kernel ABI fix & Multicast routing for Amlogic ARM64 kernel running 32-bit userspace
    # (Prevents atomic CAS spinlock in glibc std::call_once that hangs ZeroConf HTTP server)
    echo 2 > /proc/sys/abi/cp15_barrier 2>/dev/null || true
    ifconfig p2p0 down 2>/dev/null || true
    ip route add 224.0.0.0/4 dev wlan0 2>/dev/null || true

    # 1. Unmute amplifier & standby
    amixer cset numid=9 off 2>/dev/null || true
    amixer cset numid=189 off 2>/dev/null || true
    amixer sset 'SPK Master' on 2>/dev/null || true
    amixer cset numid=1 180 2>/dev/null || true

    # 2. Enable Lane0 Stereo Mix (Left + Right -> Mono speaker)
    amixer cset numid=148 1 2>/dev/null || true

    # 3. Disable speech DRC and Voice EQ for transparent Hi-Fi sound
    amixer sset 'SPK DRC' off 2>/dev/null || true
    amixer sset 'SPK EQ' off 2>/dev/null || true

    # 4. Clean stale IPC semaphores and dead aplay instances
    killall -9 aplay 2>/dev/null || true
    for id in $(ipcs -m 2>/dev/null | awk '{print $2}' | grep -E '^[0-9]+$'); do ipcrm -m $id 2>/dev/null || true; done
    for id in $(ipcs -s 2>/dev/null | awk '{print $2}' | grep -E '^[0-9]+$'); do ipcrm -s $id 2>/dev/null || true; done
}

configure_320kbps() {
    mkdir -p "$DATA_DIR/settings"
    for pref in "$DATA_DIR/settings/prefs" $(find "$DATA_DIR/settings" -name "prefs" 2>/dev/null); do
        if [ -f "$pref" ]; then
            grep -q 'audio.play_bitrate_enumeration=2' "$pref" 2>/dev/null || {
                printf '\naudio.play_bitrate_enumeration=2\naudio.play_bitrate_non_metered_enumeration=2\naudio.sync_bitrate_enumeration=2\n' >> "$pref"
            }
        fi
    done
}

start() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "ℹ️ Soloist is already running (PID: $(cat "$PID_FILE"))"
        return 0
    fi

    API_KEY="$(get_api_key)"
    if [ -z "$API_KEY" ]; then
        echo "❌ Ошибка: API ключ не найден!" >&2
        echo "Создайте файл $KEY_FILE со своим spak_... ключом или экспортируйте SOLOIST_API_KEY" >&2
        exit 1
    fi

    mkdir -p "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
    setup_audio_hardware
    configure_320kbps

    echo "🎵 Запуск Spotify Soloist ('$DEVICE_NAME')..."

    export PULSE_SERVER="unix:/tmp/soloist-pulse.sock"

    # Ensure /data/soloist symlink exists for ELF interpreter compatibility
    [ -e /data/soloist ] || ln -sf "$INSTALL_DIR" /data/soloist 2>/dev/null || true

    RUN_CMD="$BIN"
    if ! "$BIN" --version >/dev/null 2>&1; then
        RUN_CMD="$INTERP --library-path $GLIBC:$LIB $BIN"
    fi

    setsid $RUN_CMD \
        -n "$DEVICE_NAME" \
        -k "$API_KEY" \
        -D "$DATA_DIR" \
        -C "$CACHE_DIR" \
        -z 256 \
        -i 20 \
        -w 127.0.0.1:8989 \
        -v </dev/null >> "$LOG_FILE" 2>&1 &

    PID=$!
    echo "$PID" > "$PID_FILE"
    sleep 2

    if kill -0 "$PID" 2>/dev/null; then
        echo "✅ Soloist успешно запущен! PID: $PID"
        if [ -x "$INSTALL_DIR/voice.sh" ]; then
            "$INSTALL_DIR/voice.sh" run >/dev/null 2>&1 &
        fi
        echo "📱 Откройте Spotify на телефоне/ПК и выберите '$DEVICE_NAME'"
    else
        echo "❌ Ошибка запуска. Проверьте лог: cat $LOG_FILE"
        exit 1
    fi
}

start_fg() {
    API_KEY="$(get_api_key)"
    if [ -z "$API_KEY" ]; then
        echo "❌ Ошибка: API ключ не найден!" >&2
        echo "Создайте файл $KEY_FILE со своим spak_... ключом или экспортируйте SOLOIST_API_KEY" >&2
        exit 1
    fi

    mkdir -p "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
    setup_audio_hardware
    configure_320kbps

    echo "🎵 [YAM / Foreground] Запуск Spotify Soloist ('$DEVICE_NAME')..."
    export PULSE_SERVER="unix:/tmp/soloist-pulse.sock"

    if [ -x "$INSTALL_DIR/voice.sh" ]; then
        "$INSTALL_DIR/voice.sh" run >/dev/null 2>&1 &
    fi

    [ -e /data/soloist ] || ln -sf "$INSTALL_DIR" /data/soloist 2>/dev/null || true

    RUN_CMD="$BIN"
    if ! "$BIN" --version >/dev/null 2>&1; then
        RUN_CMD="$INTERP --library-path $GLIBC:$LIB $BIN"
    fi

    exec $RUN_CMD \
        -n "$DEVICE_NAME" \
        -k "$API_KEY" \
        -D "$DATA_DIR" \
        -C "$CACHE_DIR" \
        -z 256 \
        -i 20 \
        -w 127.0.0.1:8989 \
        -v
}

stop() {
    echo "🛑 Остановка Spotify Soloist..."
    if [ -f "$PID_FILE" ]; then
        PID="$(cat "$PID_FILE")"
        kill "$PID" 2>/dev/null || true
        rm -f "$PID_FILE"
    fi
    killall -9 soloist aplay voice.sh 2>/dev/null || true
    echo "✅ Остановлено."
}

status() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "● Soloist активен (PID: $(cat "$PID_FILE"), Устройство: '$DEVICE_NAME')"
        ctl now 2>/dev/null || true
    else
        echo "○ Soloist остановлен"
        exit 1
    fi
}

ctl() {
    [ -e /data/soloist ] || ln -sf "$INSTALL_DIR" /data/soloist 2>/dev/null || true
    if "$BIN" --version >/dev/null 2>&1; then
        exec "$BIN" ctl -w 127.0.0.1:8989 -D "$DATA_DIR" "$@"
    else
        exec "$INTERP" --library-path "$GLIBC:$LIB" "$BIN" ctl -w 127.0.0.1:8989 -D "$DATA_DIR" "$@"
    fi
}

log() {
    if [ -f "$LOG_FILE" ]; then
        tail -n 50 -f "$LOG_FILE"
    else
        echo "Лог-файл ещё не создан: $LOG_FILE"
    fi
}

case "${1:-}" in
    start)    start ;;
    start_fg) start_fg ;;
    stop)     stop ;;
    restart) stop; sleep 1; start ;;
    status)  status ;;
    ctl)     shift; ctl "$@" ;;
    log)     log ;;
    *)
        echo "Использование: $0 {start|stop|restart|status|ctl [команда]|log}"
        echo "Примеры:"
        echo "  $0 start              # Запустить сервис"
        echo "  $0 stop               # Остановить"
        echo "  $0 status             # Текущий статус"
        echo "  $0 ctl play           # Возобновить музыку"
        echo "  $0 ctl pause          # Поставить на паузу"
        echo "  $0 ctl next           # Следующий трек"
        echo "  $0 ctl volume 30      # Выставить громкость 30%"
        echo "  $0 log                # Просмотр логов в реальном времени"
        exit 2
        ;;
esac
