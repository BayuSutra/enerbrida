import json

import paho.mqtt.client as mqtt

from django.core.management.base import BaseCommand

from monitoring.models import SensorData


# ============================================================
# MQTT CONFIGURATION
# ============================================================

MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883

MQTT_TOPIC = "Bangkit/ESP32-001/sensor/#"


# ============================================================
# MQTT SUBSCRIBER
# ============================================================

class Command(BaseCommand):

    help = "Menjalankan MQTT subscriber untuk menerima data ESP32"

    def handle(self, *args, **options):

        self.stdout.write(
            self.style.SUCCESS(
                "========================================"
            )
        )

        self.stdout.write(
            self.style.SUCCESS(
                "     MQTT SUBSCRIBER DJANGO"
            )
        )

        self.stdout.write(
            self.style.SUCCESS(
                "========================================"
            )
        )

        self.stdout.write(
            f"Broker : {MQTT_BROKER}"
        )

        self.stdout.write(
            f"Port   : {MQTT_PORT}"
        )

        self.stdout.write(
            f"Topic  : {MQTT_TOPIC}"
        )

        self.stdout.write("")

        # ====================================================
        # CREATE MQTT CLIENT
        # ====================================================

        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="Django-ESP32-001"
        )

        # Callback ketika connect
        client.on_connect = self.on_connect

        # Callback ketika menerima message
        client.on_message = self.on_message

        # ====================================================
        # CONNECT TO BROKER
        # ====================================================

        try:

            self.stdout.write(
                "Menghubungkan ke MQTT broker..."
            )

            client.connect(
                MQTT_BROKER,
                MQTT_PORT,
                60
            )

        except Exception as e:

            self.stdout.write(
                self.style.ERROR(
                    f"Gagal koneksi MQTT: {e}"
                )
            )

            return

        # ====================================================
        # START MQTT LOOP
        # ====================================================

        self.stdout.write(
            self.style.SUCCESS(
                "MQTT loop dimulai..."
            )
        )

        client.loop_forever()


    # ========================================================
    # ON CONNECT
    # ========================================================

    def on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties
    ):

        if reason_code == 0:

            self.stdout.write(
                self.style.SUCCESS(
                    "✓ Berhasil terhubung ke MQTT Broker"
                )
            )

            # Subscribe ke semua sensor ESP32-001

            result, mid = client.subscribe(
                MQTT_TOPIC
            )

            if result == mqtt.MQTT_ERR_SUCCESS:

                self.stdout.write(
                    self.style.SUCCESS(
                        f"✓ Subscribe berhasil: {MQTT_TOPIC}"
                    )
                )

            else:

                self.stdout.write(
                    self.style.ERROR(
                        f"✗ Gagal subscribe: {result}"
                    )
                )

        else:

            self.stdout.write(
                self.style.ERROR(
                    f"✗ Gagal koneksi MQTT: {reason_code}"
                )
            )


    # ========================================================
    # ON MESSAGE
    # ========================================================

    def on_message(
        self,
        client,
        userdata,
        msg,
        properties=None
    ):

        try:

            # =================================================
            # AMBIL PAYLOAD
            # =================================================

            payload = msg.payload.decode(
                "utf-8"
            )

            print("")
            print("========================================")
            print("          MQTT DATA DITERIMA")
            print("========================================")

            print(
                "Topic   :",
                msg.topic
            )

            print(
                "Payload :",
                payload
            )


            # =================================================
            # PARSE JSON
            # =================================================

            data = json.loads(
                payload
            )


            print(
                "JSON    :",
                data
            )


            # =================================================
            # DEVICE ID
            # =================================================

            device_id = data.get(
                "device_id",
                "UNKNOWN"
            )


            # =================================================
            # SYSTEM
            # =================================================

            system = data.get(
                "system"
            )


            # =================================================
            # DETEKSI SYSTEM DARI TOPIC
            # =================================================

            if (
                system is None
                or system == ""
            ):

                if "/PLTS" in msg.topic:

                    system = "PLTS"

                elif "/PLTB" in msg.topic:

                    system = "PLTB"

                elif "/BATTERY" in msg.topic.upper() or "/BATERAI" in msg.topic.upper() or "/AKI" in msg.topic.upper() or "/SOC" in msg.topic.upper():

                    system = "BATTERY"


            # =================================================
            # VALIDASI SYSTEM
            # =================================================

            if system.upper() in ["BATTERY", "BATERAI", "AKI", "SOC"]:
                system = "BATTERY"

            if system not in [
                "PLTS",
                "PLTB",
                "BATTERY"
            ]:

                print(
                    "⚠ System tidak dikenal:",
                    system
                )

                return


            # =================================================
            # VOLTAGE
            # =================================================

            voltage = float(
                data.get(
                    "voltage",
                    0
                )
            )


            # =================================================
            # CURRENT
            # =================================================

            current = float(
                data.get(
                    "current",
                    0
                )
            )


            # =================================================
            # POWER
            # =================================================

            power_value = data.get(
                "power"
            )


            if power_value is None:

                power = (
                    voltage *
                    current
                )

            else:

                power = float(
                    power_value
                )


            # =================================================
            # SOC (STATE OF CHARGE)
            # =================================================

            soc = float(
                data.get(
                    "soc",
                    data.get("batt_soc", 0.0)
                ) or 0.0
            )


            # =================================================
            # PRINT DATA
            # =================================================

            print("")
            print("----------------------------------------")
            print(
                "Device  :",
                device_id
            )

            print(
                "System  :",
                system
            )

            print(
                "Voltage :",
                voltage,
                "V"
            )

            print(
                "Current :",
                current,
                "A"
            )

            print(
                "Power   :",
                power,
                "W"
            )

            if soc > 0 or system == "BATTERY":
                print(
                    "SOC     :",
                    soc,
                    "%"
                )

            print("----------------------------------------")


            # =================================================
            # SAVE DATABASE
            # =================================================

            SensorData.objects.create(

                device_id=device_id,

                system=system,

                voltage=voltage,

                current=current,

                power=power,

                soc=soc,

            )


            print("")
            print(
                "✓ DATA BERHASIL DISIMPAN KE DATABASE"
            )

            print(
                "========================================"
            )


        except json.JSONDecodeError:

            print(
                "✗ ERROR: Payload bukan JSON valid"
            )

            print(
                "Payload:",
                msg.payload
            )


        except ValueError as e:

            print(
                "✗ ERROR: Nilai sensor tidak valid:",
                e
            )


        except Exception as e:

            print(
                "✗ ERROR MQTT:",
                e
            )