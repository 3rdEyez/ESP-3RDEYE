import configparser
import os

# Create a ConfigParser object
config = configparser.ConfigParser()

# Read the configuration file with UTF-8 encoding
config.read('config.ini', encoding='utf-8')

# Get the COM port
esp_com_port = config.get('DEFAULT', 'ESP_COM_PORT')

# Output the read values
print(f"ESP COM Port: {esp_com_port}")

# Flash the config
ESP_TOOL_EXE = os.path.abspath('esptool.exe')
lines = open('config.ini', 'r', encoding='utf-8').readlines()
out_lines = []
for line in lines:
    if line.startswith('['):
        continue
    if line.startswith('#'):
        continue
    if line.startswith('ESP_COM_PORT'):
        continue
    if line == '\n':
        continue
    out_lines.append(line)
open('config.bin', 'wb').write(''.join(out_lines).encode('utf8') + b'\0')
os.system(f'{ESP_TOOL_EXE} --chip esp32c3 --port {esp_com_port} write_flash 0x300000 config.bin')
