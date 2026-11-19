#!/bin/sh
# Build SMSforfun.app (overlay app F4HWN v7.0.0) - lancé par ../../../compile-app.sh
# depuis App/apps/smsfriends/. Le code remplit presque tout l'overlay de 4 Kio :
# le .app livré est compilé avec clang -Oz (+ ld.lld), plus compact que GCC.
set -e

APP=smsfriends
APP_NAME="SMS for fun"
APP_VER="2.3"
APP_API_MIN=4                     # F4HWN v7.0.0 = API level 4
APP_VMA=${APP_VMA:-0x20000280}

python3 ./gen_assets.py ${APP}_assets.bin   # textes et séquences radio en flash

WARN="-Wall -Wextra"
COMMON="-mcpu=cortex-m0plus -mthumb -std=gnu11 -ffreestanding -fno-builtin -fno-common \
  -fomit-frame-pointer -ffunction-sections -fdata-sections"

if command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1; then
    clang --target=thumbv6m-none-eabi $COMMON -Oz -fno-inline-functions -fno-jump-tables \
        $WARN -c ${APP}_app.c -o ${APP}.o
    ld.lld -T app.ld --defsym=APP_VMA=$APP_VMA --gc-sections -Map=${APP}.map \
        -o ${APP}.elf ${APP}.o
    OBJCOPY=llvm-objcopy
    command -v $OBJCOPY >/dev/null 2>&1 || OBJCOPY=arm-none-eabi-objcopy
else
    # GCC : peut dépasser les 4 Kio (le linker le signale alors)
    CC=/opt/toolchain/bin/arm-none-eabi-gcc
    OBJCOPY=/opt/toolchain/bin/arm-none-eabi-objcopy
    [ -x "$CC" ] || CC=arm-none-eabi-gcc
    [ -x "$OBJCOPY" ] || OBJCOPY=arm-none-eabi-objcopy
    $CC $COMMON -Os -fno-tree-loop-distribute-patterns $WARN \
        -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--build-id=none \
        -Wl,--defsym=APP_VMA=$APP_VMA -T app.ld -Wl,-Map=${APP}.map \
        ${APP}_app.c -lgcc -o ${APP}.elf
fi

$OBJCOPY -O binary -j .app ${APP}.elf ${APP}.bin
echo "$APP_NAME: $(wc -c < ${APP}.bin) octets de code"

python3 ../pack_app.py --name "$APP_NAME" --ver "$APP_VER" \
    --api-min $APP_API_MIN --vma $APP_VMA --assets ${APP}_assets.bin \
    ${APP}.bin "$(echo "$APP_NAME" | tr -d ' ').app"
