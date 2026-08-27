import csv
from collections import defaultdict
from django.http import HttpResponse, JsonResponse
from django.shortcuts import render
from django.utils import timezone

from .models import SensorData


def dashboard(request):

    return render(
        request,
        "monitoring/dashboard.html"
    )


def export_csv(request):
    """
    Export data telemetri historis gabungan ke file format CSV lengkap dengan
    indikator pengisian baterai (Dicas oleh PLTS, Dicas oleh PLTB, atau Tidak Charge).
    """
    response = HttpResponse(content_type="text/csv; charset=utf-8")
    filename = f"monitoring_energi_{timezone.now().strftime('%Y%m%d_%H%M%S')}.csv"
    response["Content-Disposition"] = f'attachment; filename="{filename}"'

    writer = csv.writer(response)
    writer.writerow([
        "Waktu (Timestamp)",
        "Tegangan PLTS (V)",
        "Arus PLTS (A)",
        "Daya PLTS (W)",
        "Tegangan PLTB (V)",
        "Arus PLTB (A)",
        "Daya PLTB (W)",
        "Tegangan Baterai (V)",
        "Arus Baterai (A)",
        "Daya Baterai (W)",
        "Status Relay",
        "Indikator Pengisian Baterai",
    ])

    # Ambil seluruh record sensor dan kelompokkan per detik timestamp
    records = SensorData.objects.all().order_by("-timestamp")
    grouped = defaultdict(dict)

    for item in records:
        time_key = item.timestamp.strftime("%Y-%m-%d %H:%M:%S")
        grouped[time_key][item.system] = item

    MIN_VOLTAGE = 24.0

    for time_str, systems in grouped.items():
        plts = systems.get("PLTS")
        pltb = systems.get("PLTB")
        batt = systems.get("BATTERY") or systems.get("BATERAI") or systems.get("AKI")

        plts_v = plts.voltage if plts else 0.0
        plts_i = plts.current if plts else 0.0
        plts_p = plts.power if plts else 0.0

        pltb_v = pltb.voltage if pltb else 0.0
        pltb_i = pltb.current if pltb else 0.0
        pltb_p = pltb.power if pltb else 0.0

        batt_v = batt.voltage if batt else 0.0
        batt_i = batt.current if batt else 0.0
        batt_p = batt.power if batt else 0.0

        # Logika Pengisian Baterai & Switching (Syarat Masukan Minimal 24.0V)
        plts_charging = (plts_v >= MIN_VOLTAGE and plts_p > 0)
        pltb_charging = (pltb_v >= MIN_VOLTAGE and pltb_p > 0)

        if plts_charging and pltb_charging:
            if plts_p >= pltb_p:
                status_charge = "Dicas oleh PLTS"
                status_relay = "PLTS (1)"
            else:
                status_charge = "Dicas oleh PLTB"
                status_relay = "PLTB (0)"
        elif plts_charging:
            status_charge = "Dicas oleh PLTS"
            status_relay = "PLTS (1)"
        elif pltb_charging:
            status_charge = "Dicas oleh PLTB"
            status_relay = "PLTB (0)"
        else:
            status_charge = "Tidak Charge"
            status_relay = "Standby (0)"

        writer.writerow([
            time_str,
            f"{plts_v:.2f}",
            f"{plts_i:.2f}",
            f"{plts_p:.2f}",
            f"{pltb_v:.2f}",
            f"{pltb_i:.2f}",
            f"{pltb_p:.2f}",
            f"{batt_v:.2f}",
            f"{batt_i:.2f}",
            f"{batt_p:.2f}",
            status_relay,
            status_charge,
        ])

    return response


def latest_data(request):
    now = timezone.now()
    MAX_TIMEOUT_SECONDS = 5.0

    plts = SensorData.objects.filter(
        system="PLTS"
    ).order_by(
        "-timestamp"
    ).first()

    pltb = SensorData.objects.filter(
        system="PLTB"
    ).order_by(
        "-timestamp"
    ).first()

    battery = SensorData.objects.filter(
        system__in=["BATTERY", "BATERAI", "AKI", "SOC"]
    ).order_by(
        "-timestamp"
    ).first()

    # Cek record paling baru di database untuk menentukan status keaktifan ESP32
    latest_record = SensorData.objects.order_by("-timestamp").first()
    is_online = False
    time_since_last_sec = 999.0

    if latest_record:
        time_since_last_sec = (now - latest_record.timestamp).total_seconds()
        if time_since_last_sec <= MAX_TIMEOUT_SECONDS:
            is_online = True

    data = {
        "is_online": is_online,
        "timeout_seconds": MAX_TIMEOUT_SECONDS,
        "time_since_last_sec": round(time_since_last_sec, 2),
        "PLTS": None,
        "PLTB": None,
        "BATTERY": None,
        "battery": None,
        "relay": None,
        "min_voltage_threshold": 24.0,
    }

    # Jika ESP32 aktif dalam 5 detik terakhir, kirim data sensor aktual
    if is_online:
        if plts and (now - plts.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS:
            data["PLTS"] = {
                "voltage": plts.voltage,
                "current": plts.current,
                "power": plts.power,
                "timestamp": plts.timestamp.isoformat(),
            }

        if pltb and (now - pltb.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS:
            data["PLTB"] = {
                "voltage": pltb.voltage,
                "current": pltb.current,
                "power": pltb.power,
                "timestamp": pltb.timestamp.isoformat(),
            }

        if battery and (now - battery.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS:
            battery_data = {
                "voltage": battery.voltage,
                "current": battery.current,
                "power": battery.power,
                "timestamp": battery.timestamp.isoformat(),
            }
            data["BATTERY"] = battery_data
            data["battery"] = battery_data

        # Logika Switching Relay (Syarat Tegangan Minimal 24.0V)
        MIN_VOLTAGE = 24.0
        plts_p = data["PLTS"]["power"] if data["PLTS"] else 0.0
        pltb_p = data["PLTB"]["power"] if data["PLTB"] else 0.0
        plts_v = data["PLTS"]["voltage"] if data["PLTS"] else 0.0
        pltb_v = data["PLTB"]["voltage"] if data["PLTB"] else 0.0

        plts_siap = (plts_v >= MIN_VOLTAGE)
        pltb_siap = (pltb_v >= MIN_VOLTAGE)

        if plts_siap and pltb_siap:
            relay_status = 1 if plts_p >= pltb_p else 0
        elif plts_siap:
            relay_status = 1
        elif pltb_siap:
            relay_status = 0
        else:
            relay_status = None # Keduanya < 24V

        data["relay"] = relay_status

    return JsonResponse(data)