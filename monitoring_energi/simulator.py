import json
import random
import time

import paho.mqtt.client as mqtt


# ============================================================
# MQTT CONFIG
# ============================================================

MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883

DEVICE_ID = "ESP32-001"

MQTT_TOPIC_PLTS = "Bangkit/ESP32-001/sensor/PLTS"
MQTT_TOPIC_PLTB = "Bangkit/ESP32-001/sensor/PLTB"
MQTT_TOPIC_BATTERY = "Bangkit/ESP32-001/sensor/BATTERY"


# ============================================================
# MQTT CONNECT
# ============================================================

client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="DUMMY-ESP32-001"
)


print("=" * 60)
print("       DUMMY MQTT PUBLISHER")
print("=" * 60)

print(f"Broker : {MQTT_BROKER}")
print(f"Port   : {MQTT_PORT}")
print(f"Device : {DEVICE_ID}")
print()


try:

    client.connect(
        MQTT_BROKER,
        MQTT_PORT,
        60
    )

except Exception as e:

    print("Gagal terhubung ke MQTT broker:")
    print(e)

    exit()


client.loop_start()

print("✓ Berhasil terhubung ke MQTT broker")
print()
print("Mulai mengirim data dummy...")
print("Tekan CTRL+C untuk berhenti.")
print()


# ============================================================
# MAIN LOOP
# ============================================================

try:

    while True:

        # ====================================================
        # DATA PLTS
        # ====================================================

        plts_voltage = round(
            random.uniform(20.0, 28.0),
            2
        )

        plts_current = round(
            random.uniform(1.0, 8.0),
            2
        )

        plts_power = round(
            plts_voltage * plts_current,
            2
        )


        plts_data = {

            "device_id": DEVICE_ID,

            "system": "PLTS",

            "voltage": plts_voltage,

            "current": plts_current,

            "power": plts_power

        }


        # ====================================================
        # DATA PLTB
        # ====================================================

        pltb_voltage = round(
            random.uniform(11.0, 14.0),
            2
        )

        pltb_current = round(
            random.uniform(0.5, 5.0),
            2
        )

        pltb_power = round(
            pltb_voltage * pltb_current,
            2
        )


        pltb_data = {

            "device_id": DEVICE_ID,

            "system": "PLTB",

            "voltage": pltb_voltage,

            "current": pltb_current,

            "power": pltb_power

        }


        # ====================================================
        # DATA BATTERY & SOC
        # ====================================================

        battery_voltage = round(
            random.uniform(12.4, 13.8),
            2
        )

        battery_current = round(
            random.uniform(1.2, 4.5),
            2
        )

        battery_power = round(
            battery_voltage * battery_current,
            2
        )

        # Simulasi SOC 75% - 95%
        battery_soc = round(
            random.uniform(75.0, 95.0),
            1
        )

        battery_data = {
            "device_id": DEVICE_ID,
            "system": "BATTERY",
            "voltage": battery_voltage,
            "current": battery_current,
            "power": battery_power,
            "soc": battery_soc
        }


        # ====================================================
        # PUBLISH PLTS
        # ====================================================

        client.publish(

            MQTT_TOPIC_PLTS,

            json.dumps(
                plts_data
            )

        )


        # ====================================================
        # PUBLISH PLTB
        # ====================================================

        client.publish(

            MQTT_TOPIC_PLTB,

            json.dumps(
                pltb_data
            )

        )


        # ====================================================
        # PUBLISH BATTERY
        # ====================================================

        client.publish(

            MQTT_TOPIC_BATTERY,

            json.dumps(
                battery_data
            )

        )


        # ====================================================
        # DISPLAY
        # ====================================================

        print(
            f"PLTS | "
            f"V={plts_voltage:6.2f} V | "
            f"I={plts_current:6.2f} A | "
            f"P={plts_power:7.2f} W"
        )

        print(
            f"PLTB | "
            f"V={pltb_voltage:6.2f} V | "
            f"I={pltb_current:6.2f} A | "
            f"P={pltb_power:7.2f} W"
        )

        print(
            f"AKI  | "
            f"V={battery_voltage:6.2f} V | "
            f"I={battery_current:6.2f} A | "
            f"SOC={battery_soc:5.1f} %"
        )

        print("-" * 60)


        # ====================================================
        # INTERVAL
        # ====================================================

        time.sleep(2)


except KeyboardInterrupt:

    print()
    print("Dummy publisher dihentikan.")


finally:

    client.loop_stop()

    client.disconnect()

    print("MQTT disconnected.")