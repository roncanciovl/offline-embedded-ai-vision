Write-Host "Setting target to ESP32-P4..."
idf.py set-target esp32p4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Building the project..."
idf.py build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Flashing and starting serial monitor..."
idf.py flash monitor
