# Compila src/ (ESP-IDF, C puro) para o Wokwi usando o Arduino CLI, sem alterar o código.
# shortcut: o app_main é renomeado para robo_main e chamado pelo setup() do Arduino (core 3.x = ESP-IDF 5.x),
# com os logs INFO ligados como no ESP-IDF padrão;
# para gravar no robô de verdade, compile com ESP-IDF/PlatformIO.
$ErrorActionPreference = 'Stop'
$sk = Join-Path $env:TEMP 'robo_idf_build\robo_idf'
New-Item -ItemType Directory -Force $sk | Out-Null
Copy-Item "$PSScriptRoot\src\*" $sk -Force
Set-Content "$sk\robo_idf.ino" -Encoding ascii -Value 'extern "C" void robo_main(void);', '#include "esp_log.h"', 'void setup() { esp_log_level_set("*", ESP_LOG_INFO); robo_main(); }', 'void loop() {}'
& "C:\Program Files\Arduino CLI\arduino-cli.exe" compile --fqbn esp32:esp32:esp32:DebugLevel=info --warnings all `
  --build-property "compiler.c.extra_flags=-Dapp_main=robo_main -DLOG_LOCAL_LEVEL=ESP_LOG_INFO" --output-dir "$PSScriptRoot\build" $sk
