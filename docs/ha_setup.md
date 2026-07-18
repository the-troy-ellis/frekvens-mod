# Home Assistant Setup

Two integration paths are available. Use whichever suits your workflow.

---

## Option A — Custom Firmware + MQTT (recommended)

The PlatformIO firmware exposes the device via MQTT autodiscovery. It registers
a `light` entity automatically and listens on additional topics for display commands.

### 1. Flash the ESP32

```bash
cd esp32
pio run --target upload          # initial USB flash
pio run --target uploadfs        # upload web UI to LittleFS
```

On first boot the ESP32 creates an AP named **Frekvens-Setup**. Connect to it, enter
your WiFi credentials, and configure the MQTT broker address in the web UI at
`http://frekvens.local`.

### 2. MQTT broker configuration

Set the following in the web UI **Settings** panel (or POST to `/api/config`):

| Field | Example |
|---|---|
| MQTT broker | `192.168.1.10` |
| MQTT port | `1883` |
| MQTT user | *(leave blank if none)* |
| Enable MQTT | ✓ |

### 3. Home Assistant — MQTT integration

Add the MQTT integration in HA if not already configured:
**Settings → Devices & Services → Add Integration → MQTT**

The Frekvens device will appear automatically via MQTT discovery as a **light** entity
with brightness support. No YAML configuration needed.

### 4. Topics reference

| Topic | Direction | Payload | Purpose |
|---|---|---|---|
| `frekvens/state` | ESP32 → HA | `{"state":"ON","brightness":15}` | Current state |
| `frekvens/set` | HA → ESP32 | `{"state":"ON","brightness":8}` | On/off + brightness |
| `frekvens/display/set` | HA → ESP32 | JSON (see below) | Display content |
| `frekvens/availability` | ESP32 → HA | `online` / `offline` | LWT |

#### `frekvens/display/set` payload schema

```json
{
  "mode": "text",
  "text": "Hello World",
  "brightness": 15,
  "scroll_ms": 80
}
```

```json
{
  "mode": "image",
  "name": "logo.raw",
  "brightness": 12
}
```

```json
{
  "mode": "animation",
  "name": "bounce.anim",
  "frame_ms": 100,
  "loop": true
}
```

```json
{ "mode": "off" }
```

---

## Home Assistant automation examples

### Display time + temperature on a trigger

```yaml
automation:
  - alias: "Frekvens — show temperature"
    trigger:
      - platform: state
        entity_id: sensor.outdoor_temperature
    action:
      - service: mqtt.publish
        data:
          topic: frekvens/display/set
          payload_template: >
            {"mode":"text","text":"{{ states('sensor.outdoor_temperature') }}°C",
             "brightness":12,"scroll_ms":100}
```

### Turn off display at night

```yaml
automation:
  - alias: "Frekvens — night off"
    trigger:
      - platform: time
        at: "23:00:00"
    action:
      - service: light.turn_off
        target:
          entity_id: light.frekvens_array
```

### Show an animation when doorbell rings

```yaml
automation:
  - alias: "Frekvens — doorbell animation"
    trigger:
      - platform: state
        entity_id: binary_sensor.doorbell
        to: "on"
    action:
      - service: mqtt.publish
        data:
          topic: frekvens/display/set
          payload: '{"mode":"animation","name":"alert.anim","frame_ms":80,"loop":false}'
      - delay: "00:00:10"
      - service: mqtt.publish
        data:
          topic: frekvens/display/set
          payload: '{"mode":"off"}'
```

### Lovelace button card

```yaml
type: button
name: Frekvens
icon: mdi:television
entity: light.frekvens_array
tap_action:
  action: toggle
hold_action:
  action: call-service
  service: mqtt.publish
  service_data:
    topic: frekvens/display/set
    payload: '{"mode":"text","text":"HA!","brightness":15,"scroll_ms":60}'
```

### HA Script — display arbitrary text

```yaml
script:
  frekvens_text:
    alias: "Frekvens display text"
    fields:
      message:
        description: "Text to scroll"
        example: "Hello"
      brightness:
        description: "0-15"
        example: 15
    sequence:
      - service: mqtt.publish
        data:
          topic: frekvens/display/set
          payload_template: >
            {"mode":"text","text":"{{ message }}",
             "brightness":{{ brightness | default(15) }},"scroll_ms":80}
```

---

## Hardware test endpoints

Before connecting to HA, verify your hardware using the ESP32 web API:

| Endpoint | Action |
|---|---|
| `GET /api/test/chain` | Scrolls "TEST" across all panels |
| `GET /api/test/sweep` | Column sweep — one lit column advancing, loops |
| `GET /api/test/pixel?x=0&y=0&v=15` | Lights a single pixel at (x, y) with brightness v |

Example (curl):
```bash
curl http://frekvens.local/api/test/sweep
curl "http://frekvens.local/api/test/pixel?x=8&y=8&v=15"
```

---

## Option B — ESPHome

Use this if you prefer the ESPHome ecosystem and don't need the full web UI.

### 1. Prerequisites

```bash
pip install esphome
```

Copy `secrets.yaml` next to the YAML file:

```yaml
# esphome/secrets.yaml
wifi_ssid: "YourWiFi"
wifi_password: "YourPassword"
api_key: "your-32-byte-base64-api-key"
ota_password: "frekvens-ota"
```

Generate an API key:
```bash
python3 -c "import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())"
```

### 2. Flash

```bash
cd esphome
esphome run frekvens_array.yaml
```

The device registers with HA via the **ESPHome integration** (not MQTT).
It exposes a `light` entity for brightness and the following callable services:

| HA Service | Parameters |
|---|---|
| `esphome.frekvens_array_display_text` | `text`, `scroll_ms`, `brightness` |
| `esphome.frekvens_array_display_off` | *(none)* |

### 3. Edit `frekvens_array.yaml` for panel count

Change the `num_panels` substitution at the top of the file to match your chain:

```yaml
substitutions:
  num_panels: "2"   # 1–4
```

### 4. Change WiFi credentials after flashing

Hold the on-board BOOT button for 3 seconds at startup to launch the captive-portal
provisioning AP (custom firmware only). For ESPHome, re-flash with updated `secrets.yaml`.

---

## OTA firmware updates

```bash
# Custom firmware
cd esp32
pio run --env esp32dev_ota --target upload   # uploads over WiFi to frekvens.local

# ESPHome
cd esphome
esphome run frekvens_array.yaml              # re-compiles and uploads OTA automatically
```
