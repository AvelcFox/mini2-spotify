#!/bin/sh
# 📦 Установщик Spotify Soloist для Яндекс Станции Мини 2 (и других ARMv7 колонок)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET_DIR="/data/spotify"
INSTALL_TARGET="${1:-adb}"

echo "=================================================="
echo " 🎵 Spotify Soloist: Установщик для Станции Мини 2"
echo "=================================================="

# 1. Проверка API ключа
if [ -z "${SOLOIST_API_KEY:-}" ] && [ ! -s "$SCRIPT_DIR/api_key.txt" ]; then
    printf "🔑 Введите ваш Spotify Soloist API Key (spak_...): "
    read -r API_KEY
    if [ -z "$API_KEY" ]; then
        echo "❌ Ключ не может быть пустым. Получить можно на: https://developer.spotify.com/dashboard/soloist"
        exit 1
    fi
    printf '%s\n' "$API_KEY" > "$SCRIPT_DIR/api_key.txt"
fi

# 2. Имя колонки
if [ -z "${DEVICE_NAME:-}" ] && [ ! -s "$SCRIPT_DIR/device_name.txt" ]; then
    printf "🏷️  Введите имя колонки в Spotify [Яндекс Мини 2]: "
    read -r D_NAME
    [ -n "$D_NAME" ] || D_NAME="Яндекс Мини 2"
    printf '%s\n' "$D_NAME" > "$SCRIPT_DIR/device_name.txt"
fi

if [ "$INSTALL_TARGET" = "adb" ]; then
    echo "📡 Установка через ADB..."
    adb shell "mkdir -p $TARGET_DIR"
    adb push "$SCRIPT_DIR/bin" "$TARGET_DIR/"
    adb push "$SCRIPT_DIR/glibc" "$TARGET_DIR/"
    adb push "$SCRIPT_DIR/lib" "$TARGET_DIR/"
    adb push "$SCRIPT_DIR/service.sh" "$TARGET_DIR/"
    [ ! -f "$SCRIPT_DIR/api_key.txt" ] || adb push "$SCRIPT_DIR/api_key.txt" "$TARGET_DIR/"
    [ ! -f "$SCRIPT_DIR/device_name.txt" ] || adb push "$SCRIPT_DIR/device_name.txt" "$TARGET_DIR/"

    adb shell "chmod +x $TARGET_DIR/bin/soloist $TARGET_DIR/service.sh $TARGET_DIR/glibc/* $TARGET_DIR/lib/*"
    
    echo "🚀 Запуск сервиса..."
    adb shell "$TARGET_DIR/service.sh restart"

elif echo "$INSTALL_TARGET" | grep -q '@'; then
    echo "📡 Установка через SSH на $INSTALL_TARGET..."
    ssh "$INSTALL_TARGET" "mkdir -p $TARGET_DIR"
    scp -r "$SCRIPT_DIR/bin" "$SCRIPT_DIR/glibc" "$SCRIPT_DIR/lib" "$SCRIPT_DIR/service.sh" "$INSTALL_TARGET:$TARGET_DIR/"
    [ ! -f "$SCRIPT_DIR/api_key.txt" ] || scp "$SCRIPT_DIR/api_key.txt" "$INSTALL_TARGET:$TARGET_DIR/"
    [ ! -f "$SCRIPT_DIR/device_name.txt" ] || scp "$SCRIPT_DIR/device_name.txt" "$INSTALL_TARGET:$TARGET_DIR/"

    ssh "$INSTALL_TARGET" "chmod +x $TARGET_DIR/bin/soloist $TARGET_DIR/service.sh $TARGET_DIR/glibc/* $TARGET_DIR/lib/* && $TARGET_DIR/service.sh restart"
else
    echo "❌ Неизвестный метод установки: '$INSTALL_TARGET'. Используйте 'adb' или 'root@<IP>'"
    exit 1
fi

echo ""
echo "✨ Установка завершена!"
echo "Управление на колонке: $TARGET_DIR/service.sh {start|stop|status|ctl|log}"
