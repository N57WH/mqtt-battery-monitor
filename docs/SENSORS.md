# Battery Monitor — Sensor Output Reference

Documentation for the six sensors published by the Battery Monitor
device in Home Assistant. All values are calculated on the ESP32
and published via MQTT every 60 seconds.

---

## Sensors at a glance

| Entity | Type | Unit | Example |
|--------|------|------|---------|
| Battery Voltage | sensor | V | 12.847 |
| Battery SOC | sensor | % | 75 |
| Voltage Trend | sensor | V/hr | -0.023 |
| Battery Trend | sensor | — | Stable |
| Battery Condition | binary_sensor | — | Normal |
| Battery Chemistry | select | — | Lead-Acid (Flooded) |

---

## Battery Voltage

The raw voltage reading from the INA260 sensor. The chip measures
bus voltage at the VBus pin (tied to Vin+ on the breakout) with
a 1.25 mV resolution and 16-bit ADC. The firmware applies 64-sample
hardware averaging with 1.1 ms conversion time for a clean, stable
reading.

The value is reported in volts to three decimal places (e.g. 12.847 V).

This is the open-circuit voltage (OCV) of the battery if no load is
connected, or the terminal voltage under load if a charger or load
is present. SOC accuracy is highest when the battery has been at
rest for 15–30 minutes — surface charge from recent charging or
voltage sag from a load will skew the reading.

---

## Battery SOC (State of Charge)

Estimated percentage of charge remaining in the battery, calculated
from voltage using piecewise linear interpolation against a lookup
table matched to the selected chemistry.

### How it works

The firmware stores a table of (voltage, SOC%) data points for each
chemistry. When a voltage reading comes in, the code finds which two
table entries the voltage falls between, calculates the fractional
position, and interpolates the SOC value.

Example: a Flooded battery reading 12.42 V falls between the 12.37 V
(70%) and 12.48 V (80%) entries.

```
fraction = (12.42 - 12.37) / (12.48 - 12.37) = 0.45
SOC = 70 + (0.45 × 10) = 75%
```

Voltages above the 100% entry return 100%. Voltages below the 0%
entry return 0%.

### Lookup tables

#### Lead-Acid (Flooded)

| Resting voltage | SOC |
|----------------|-----|
| 12.73 V | 100% |
| 12.58 V | 90% |
| 12.48 V | 80% |
| 12.37 V | 70% |
| 12.28 V | 60% |
| 12.17 V | 50% |
| 12.06 V | 40% |
| 11.96 V | 30% |
| 11.81 V | 20% |
| 11.66 V | 10% |
| 11.51 V | 5% |
| 11.31 V | 0% |

#### Lead-Acid (AGM)

| Resting voltage | SOC |
|----------------|-----|
| 12.80 V | 100% |
| 12.62 V | 90% |
| 12.50 V | 80% |
| 12.42 V | 70% |
| 12.32 V | 60% |
| 12.20 V | 50% |
| 12.11 V | 40% |
| 12.06 V | 30% |
| 11.96 V | 20% |
| 11.81 V | 10% |
| 11.66 V | 5% |
| 11.51 V | 0% |

#### LiFePO4 (4S, 12.8V nominal)

| Resting voltage | SOC |
|----------------|-----|
| 13.60 V | 100% |
| 13.40 V | 95% |
| 13.35 V | 90% |
| 13.30 V | 80% |
| 13.28 V | 70% |
| 13.20 V | 50% |
| 13.10 V | 30% |
| 13.00 V | 20% |
| 12.80 V | 10% |
| 12.00 V | 5% |
| 11.20 V | 0% |

### Accuracy

Voltage-based SOC estimation has inherent limitations:

- **Lead-acid**: ±5–10%. The voltage curve is reasonably spread
  across the SOC range, giving usable resolution at every level.

- **LiFePO4**: ±10–15%. The discharge curve is very flat between
  20–80% SOC (only ~0.3 V spans 60% of capacity), making it hard
  to distinguish mid-range states from voltage alone.

- **Under load**: voltage sag causes under-reading. A battery at
  80% SOC may show 70% under heavy draw.

- **After charging**: surface charge causes over-reading. A battery
  just taken off a charger may show 100% but settle to 90% after
  15–30 minutes of rest.

### Changing chemistry

The "Battery Chemistry" select entity in Home Assistant lets you
switch between the three lookup tables at any time. The selection
is saved to the ESP32's NVS flash and persists across reboots.
SOC recalculates immediately on the next 60-second cycle.

---

## Voltage Trend

The rate of voltage change in volts per hour (V/hr), calculated
from a rolling 30-minute buffer of readings.

### How it works

The ESP32 maintains a circular buffer of 30 voltage samples, one
per minute. Each cycle the buffer is split into two halves:

```
Oldest 15 samples → average A
Newest 15 samples → average B

delta = B - A
minutes between midpoints = 15 minutes
trend = delta × (60 / 15) = delta × 4 → V/hr
```

This approach smooths out noise while still detecting genuine
trends. A single noisy reading doesn't move the needle — it takes
a sustained change over multiple minutes to shift the trend value.

### Reading the value

| Value | Meaning |
|-------|---------|
| +0.150 V/hr | Voltage rising steadily — charger active |
| +0.030 V/hr | Slight rise — trickle charge or recovery |
| -0.001 V/hr | Essentially flat — resting battery |
| -0.050 V/hr | Gradual decline — light load |
| -0.300 V/hr | Rapid drop — heavy discharge or problem |

### Startup behavior

The buffer fills gradually after boot. During the first 2 minutes
the trend reads 0.000 V/hr because there aren't enough samples to
compare. After 30 minutes the buffer is full and the trend reflects
the complete half-hour window.

### Dashboard charting

Add a History Graph card in Home Assistant to visualize the voltage
trend over time:

```yaml
type: history-graph
title: Voltage trend (24h)
hours_to_show: 24
entities:
  - entity: sensor.battery_monitor_voltage_trend
```

Overlay it with the voltage graph to correlate:

```yaml
type: history-graph
title: Battery voltage and trend
hours_to_show: 24
entities:
  - entity: sensor.battery_monitor_battery_voltage
  - entity: sensor.battery_monitor_voltage_trend
```

Useful patterns to watch for in the charts:

- **Sawtooth voltage + positive trend**: charger cycling between
  bulk and float stages. Normal for lead-acid.

- **Flat voltage + zero trend**: battery at rest, no load, no
  charger. Ideal for accurate SOC readings.

- **Declining voltage + negative trend**: active discharge.
  Steeper slope = heavier load.

- **Voltage dropping then flattening**: load was removed, battery
  recovering toward open-circuit voltage.

- **Sudden voltage drop**: large load switched on, or a cell
  failing. Check if it recovers when load is removed.

---

## Battery Trend (label)

A plain-text label derived from the voltage trend value. Easier to
read at a glance than the raw V/hr number.

| Trend rate | Label |
|-----------|-------|
| Above +0.10 V/hr | Charging fast |
| +0.02 to +0.10 V/hr | Charging |
| -0.02 to +0.02 V/hr | Stable |
| -0.10 to -0.02 V/hr | Discharging |
| Below -0.10 V/hr | Discharging fast |

The ±0.02 V/hr deadband around zero prevents the label from
flickering between "Charging" and "Discharging" due to noise
on a resting battery.

---

## Battery Condition

A binary sensor indicating whether the battery is low.

| SOC | State | HA display |
|-----|-------|------------|
| 20% or above | OFF | Normal |
| Below 20% | ON | Low |

This is a simple threshold check — `SOC < 20` → low. It does not
predict time remaining or account for discharge rate. Its primary
purpose is to trigger automations or dashboard warnings.

The 20% threshold was chosen because:

- **Lead-acid**: discharging below 20% SOC (~11.8 V) accelerates
  sulfation and shortens battery life significantly.

- **LiFePO4**: 20% SOC (~12.6 V) provides a safety margin above
  the BMS low-voltage cutoff, which is typically around 10–11 V.

---

## Dashboard examples

### Entities card

```yaml
type: entities
title: Battery monitor
entities:
  - entity: sensor.battery_monitor_battery_voltage
    name: Voltage
  - entity: sensor.battery_monitor_battery_soc
    name: SOC
  - entity: sensor.battery_monitor_battery_trend
    name: Trend
  - entity: sensor.battery_monitor_voltage_trend
    name: Trend rate
  - entity: binary_sensor.battery_monitor_battery_low
    name: Condition
  - entity: select.battery_monitor_battery_chemistry
    name: Chemistry
```

### SOC gauge

```yaml
type: gauge
entity: sensor.battery_monitor_battery_soc
name: State of charge
min: 0
max: 100
severity:
  green: 50
  yellow: 20
  red: 10
```

### Voltage history (24 hours)

```yaml
type: history-graph
title: Battery voltage
hours_to_show: 24
entities:
  - entity: sensor.battery_monitor_battery_voltage
```

### Voltage history (7 days)

```yaml
type: history-graph
title: Battery voltage (week)
hours_to_show: 168
entities:
  - entity: sensor.battery_monitor_battery_voltage
```

### SOC history

```yaml
type: history-graph
title: State of charge
hours_to_show: 24
entities:
  - entity: sensor.battery_monitor_battery_soc
```

---

## MQTT payload format

All sensors are published in a single JSON message every 60 seconds
to `homeassistant/sensor/battery_monitor/state`:

```json
{
  "voltage": 12.847,
  "soc": 75,
  "trend": -0.023,
  "trend_label": "Stable",
  "low": "OFF"
}
```

Chemistry state is published separately to
`homeassistant/select/battery_monitor_chemistry/state` and only
changes when the user selects a different chemistry in HA.
