import csv
from collections import defaultdict
from django.http import HttpResponse, JsonResponse
from django.shortcuts import render
from django.utils import timezone
from django.views.decorators.csrf import ensure_csrf_cookie

from .models import SensorData, SystemSetting, SystemLog
import json
import paho.mqtt.publish as mqtt_publish

MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883
TOPIC_CONTROL_THRESHOLD = "Bangkit/ESP32-001/control/threshold"


def log_event(category, level, message, metadata=None):
    try:
        SystemLog.objects.create(
            category=category,
            level=level,
            message=message,
            metadata=metadata
        )
    except Exception:
        pass


def get_current_threshold():
    setting, _ = SystemSetting.objects.get_or_create(
        key="voltage_threshold",
        defaults={"value": "24.0", "description": "Ambang batas tegangan minimum (V) untuk relay switching"}
    )
    try:
        return float(setting.value)
    except ValueError:
        return 24.0


def get_load_name():
    setting, _ = SystemSetting.objects.get_or_create(
        key="load_name",
        defaults={"value": "Beban", "description": "Nama beban yang ditampilkan pada dashboard"}
    )
    return setting.value.strip() or "Beban"


def set_threshold(request):
    """
    Endpoint POST untuk menyimpan threshold baru dari website
    dan meneruskannya via MQTT ke ESP32.
    """
    if request.method == "POST":
        try:
            body = json.loads(request.body.decode("utf-8")) if request.body else request.POST
            new_val = float(body.get("threshold", 24.0))
            if new_val < 0:
                log_event("WEB", "WARNING", f"Percobaan set threshold gagal: nilai {new_val} < 0")
                return JsonResponse({"status": "error", "message": "Threshold harus >= 0"}, status=400)

            setting, _ = SystemSetting.objects.get_or_create(key="voltage_threshold")
            old_val = setting.value
            setting.value = str(round(new_val, 2))
            setting.save()

            log_event("WEB", "SUCCESS", f"Threshold tegangan berhasil diubah dari {old_val}V menjadi {setting.value}V", {"old": old_val, "new": setting.value})

            # Publish threshold baru ke MQTT agar langsung ditangkap ESP32
            payload = json.dumps({"threshold": round(new_val, 2)})
            try:
                mqtt_publish.single(
                    TOPIC_CONTROL_THRESHOLD,
                    payload=payload,
                    hostname=MQTT_BROKER,
                    port=MQTT_PORT,
                    client_id="Django-Threshold-Publisher"
                )
                log_event("MQTT", "SUCCESS", f"Publish threshold {new_val}V ke broker '{MQTT_BROKER}' topik '{TOPIC_CONTROL_THRESHOLD}' berhasil")
            except Exception as mqtt_err:
                log_event("MQTT", "ERROR", f"Gagal publish threshold {new_val}V ke MQTT broker: {mqtt_err}")
                print(f"[WARN] Gagal publish threshold ke MQTT: {mqtt_err}")

            return JsonResponse({"status": "success", "threshold": round(new_val, 2)})
        except Exception as e:
            log_event("WEB", "ERROR", f"Exception pada set_threshold: {e}")
            return JsonResponse({"status": "error", "message": str(e)}, status=400)

    return JsonResponse({"status": "error", "message": "Gunakan metode POST"}, status=405)


def set_load_name(request):
    if request.method != "POST":
        return JsonResponse({"status": "error", "message": "Gunakan metode POST"}, status=405)

    try:
        body = json.loads(request.body.decode("utf-8")) if request.body else request.POST
    except (json.JSONDecodeError, UnicodeDecodeError):
        return JsonResponse({"status": "error", "message": "Format JSON tidak valid"}, status=400)

    if not isinstance(body, dict):
        return JsonResponse({"status": "error", "message": "Format data tidak valid"}, status=400)

    load_name = body.get("load_name")
    if not isinstance(load_name, str):
        return JsonResponse({"status": "error", "message": "Nama beban wajib diisi"}, status=400)

    load_name = load_name.strip()
    if not load_name or len(load_name) > 50:
        return JsonResponse(
            {"status": "error", "message": "Nama beban harus berisi 1 sampai 50 karakter"},
            status=400
        )

    setting, _ = SystemSetting.objects.get_or_create(key="load_name")
    setting.value = load_name
    setting.description = "Nama beban yang ditampilkan pada dashboard"
    setting.save()
    log_event("WEB", "SUCCESS", f"Nama beban dashboard diubah menjadi '{load_name}'")
    return JsonResponse({"status": "success", "load_name": load_name})


def api_logs(request):
    """
    Endpoint API realtime untuk mengambil log berdasarkan kategori:
    ALL, MQTT, WEB, DATABASE, DOWNLOAD, SYSTEM
    """
    category = request.GET.get("category", "ALL").upper()
    limit = int(request.GET.get("limit", 100))

    qs = SystemLog.objects.all()
    if category != "ALL":
        qs = qs.filter(category=category)

    logs = qs[:limit]
    data = []
    for log in logs:
        data.append({
            "id": log.id,
            "timestamp": log.created_at.strftime("%Y-%m-%d %H:%M:%S"),
            "category": log.category,
            "level": log.level,
            "message": log.message,
            "metadata": log.metadata,
        })

    # Tambahkan log akses viewer log secara periodik jika diinginkan
    return JsonResponse({
        "status": "success",
        "category": category,
        "count": len(data),
        "logs": data
    })


def api_clear_logs(request):
    """
    Menghapus log berdasarkan kategori atau seluruhnya
    """
    if request.method == "POST":
        category = request.POST.get("category") or (json.loads(request.body).get("category") if request.body else "ALL")
        category = category.upper()

        if category == "ALL":
            deleted_count, _ = SystemLog.objects.all().delete()
            log_event("SYSTEM", "INFO", "Seluruh System Log berhasil dibersihkan")
        else:
            deleted_count, _ = SystemLog.objects.filter(category=category).delete()
            log_event("SYSTEM", "INFO", f"System Log kategori '{category}' ({deleted_count} entri) dibersihkan")

        return JsonResponse({"status": "success", "deleted": deleted_count})
    return JsonResponse({"status": "error", "message": "POST required"}, status=405)


def trigger_ota(request):
    """
    Endpoint POST untuk mengupload file firmware (.bin) ESP32
    dan mengirim perintah OTA Update ke ESP32 via MQTT.
    """
    if request.method == "POST":
        firmware_file = request.FILES.get("firmware")
        if not firmware_file:
            log_event("SYSTEM", "WARNING", "Gagal upload OTA: Tidak ada file firmware yang dipilih")
            return JsonResponse({"status": "error", "message": "File firmware (.bin) wajib diunggah!"}, status=400)

        if not firmware_file.name.endswith(".bin"):
            log_event("SYSTEM", "WARNING", f"Gagal upload OTA: Ekstensi file {firmware_file.name} tidak valid")
            return JsonResponse({"status": "error", "message": "Format file harus berekstensi .bin!"}, status=400)

        import os
        import hashlib
        from django.conf import settings

        ota_dir = os.path.join(settings.MEDIA_ROOT, "firmware")
        os.makedirs(ota_dir, exist_ok=True)

        saved_path = os.path.join(ota_dir, "firmware_update.bin")
        md5_hash = hashlib.md5()
        with open(saved_path, "wb+") as destination:
            for chunk in firmware_file.chunks():
                destination.write(chunk)
                md5_hash.update(chunk)

        file_size_kb = round(os.path.getsize(saved_path) / 1024, 2)
        expected_md5 = md5_hash.hexdigest().lower()

        # Dapatkan IP LAN lokal yang sebenarnya agar ESP32 bisa mengakses (bukan 127.0.0.1 / localhost)
        import socket
        def get_local_ip():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                # Menggunakan dummy connect untuk mencari default outgoing route IP
                s.connect(('8.8.8.8', 80))
                ip = s.getsockname()[0]
            except Exception:
                ip = '127.0.0.1'
            finally:
                s.close()
            return ip

        host = request.get_host()
        # Jika request dibuka dari localhost / 127.0.0.1, ubah menjadi IP LAN lokal
        if "127.0.0.1" in host or "localhost" in host:
            local_ip = get_local_ip()
            port = "8000"
            if ":" in host:
                port = host.split(":")[1]
            host = f"{local_ip}:{port}"

        firmware_url = f"http://{host}/media/firmware/firmware_update.bin"

        # Kirim perintah OTA ke ESP32 via MQTT lengkap dengan MD5 Checksum
        TOPIC_CONTROL_OTA = "Bangkit/ESP32-001/control/ota"
        ota_payload = json.dumps({
            "action": "UPDATE_OTA",
            "url": firmware_url,
            "filename": firmware_file.name,
            "size_kb": file_size_kb,
            "md5": expected_md5,
            "timestamp": timezone.now().strftime("%Y-%m-%d %H:%M:%S")
        })

        try:
            mqtt_publish.single(
                TOPIC_CONTROL_OTA,
                payload=ota_payload,
                hostname=MQTT_BROKER,
                port=MQTT_PORT,
                client_id="Django-OTA-Publisher"
            )
            log_event("SYSTEM", "SUCCESS", f"OTA Trigger terkirim ke ESP32. Firmware: {firmware_file.name} ({file_size_kb} KB), MD5: {expected_md5}, URL: {firmware_url}")
            log_event("MQTT", "INFO", f"Publish OTA command ke '{TOPIC_CONTROL_OTA}' berhasil dengan checksum MD5: {expected_md5}.")
            return JsonResponse({
                "status": "success",
                "message": f"Firmware berhasil diunggah ({file_size_kb} KB, MD5: {expected_md5[:8]}...). Sinyal update aman telah dikirimkan ke ESP32!",
                "url": firmware_url,
                "md5": expected_md5
            })
        except Exception as err:
            log_event("SYSTEM", "ERROR", f"Gagal mengirim trigger OTA via MQTT: {err}")
            return JsonResponse({
                "status": "error",
                "message": f"Firmware tersimpan tetapi gagal mengirim sinyal MQTT: {err}"
            }, status=500)

    return JsonResponse({"status": "error", "message": "Method POST required"}, status=405)


def dashboard(request):
    return render(
        request,
        "monitoring/dashboard.html"
    )


@ensure_csrf_cookie
def config_view(request):
    """
    Halaman backend tersembunyi (/config) khusus admin/engineer
    untuk mengatur ambang batas (threshold) relay tegangan.
    """
    current_threshold = get_current_threshold()
    return render(
        request,
        "monitoring/config.html",
        {"current_threshold": current_threshold, "load_name": get_load_name()}
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

    records = SensorData.objects.all().order_by("-timestamp")
    grouped = defaultdict(dict)

    for item in records:
        time_key = item.timestamp.strftime("%Y-%m-%d %H:%M:%S")
        grouped[time_key][item.system] = item

    min_v = get_current_threshold()

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

        # Logika: Melebihi threshold, yang tegangan lebih tinggi terpilih
        plts_pass = (plts_v >= min_v)
        pltb_pass = (pltb_v >= min_v)

        if plts_pass and pltb_pass:
            if plts_v >= pltb_v:
                status_charge = "Dicas oleh PLTS"
                status_relay = "PLTS (0)"
            else:
                status_charge = "Dicas oleh PLTB"
                status_relay = "PLTB (1)"
        elif plts_pass:
            status_charge = "Dicas oleh PLTS"
            status_relay = "PLTS (0)"
        elif pltb_pass:
            status_charge = "Dicas oleh PLTB"
            status_relay = "PLTB (1)"
        else:
            status_charge = "Tidak Charge"
            status_relay = "Standby"

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

    log_event("DOWNLOAD", "SUCCESS", f"File CSV '{filename}' berhasil di-generate & diunduh ({len(records)} baris data diproses)")
    return response


def latest_data(request):
    now = timezone.now()
    MAX_TIMEOUT_SECONDS = 15.0
    current_threshold = get_current_threshold()

    plts = SensorData.objects.filter(system="PLTS").order_by("-timestamp").first()

    pltb = SensorData.objects.filter(system="PLTB").order_by("-timestamp").first()

    battery = (
        SensorData.objects.filter(
            system__in=["BATTERY", "BATERAI", "AKI", "SOC"]
        )
        .order_by("-timestamp")
        .first()
    )

    latest_record = SensorData.objects.order_by("-timestamp").first()
    is_online = False
    time_since_last_sec = 999.0

    if latest_record:
        time_since_last_sec = (now - latest_record.timestamp).total_seconds()
        if time_since_last_sec <= MAX_TIMEOUT_SECONDS:
            is_online = True

    # 1. Cari nilai suhu_beban dari record BATTERY terlebih dahulu, lalu PLTS
    suhu_val = None
    if battery and getattr(battery, "suhu_beban", None) is not None:
        suhu_val = battery.suhu_beban
    elif plts and getattr(plts, "suhu_beban", None) is not None:
        suhu_val = plts.suhu_beban

    # 2. Struktur utama response JSON
    data = {
        "is_online": is_online,
        "timeout_seconds": MAX_TIMEOUT_SECONDS,
        "time_since_last_sec": round(time_since_last_sec, 2),
        "PLTS": None,
        "PLTB": None,
        "BATTERY": None,
        "battery": None,
        "relay": None,
        "suhu_beban": suhu_val,  # Ditambahkan di root JSON
        "min_voltage_threshold": current_threshold,
        "load_name": get_load_name(),
    }

    if is_online:
        if plts and (now - plts.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS:
            data["PLTS"] = {
                "voltage": plts.voltage,
                "current": plts.current,
                "power": plts.power,
                "suhu_beban": getattr(plts, "suhu_beban", None),
                "timestamp": plts.timestamp.isoformat(),
            }

        if pltb and (now - pltb.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS:
            data["PLTB"] = {
                "voltage": pltb.voltage,
                "current": pltb.current,
                "power": pltb.power,
                "timestamp": pltb.timestamp.isoformat(),
            }

        if (
            battery
            and (now - battery.timestamp).total_seconds() <= MAX_TIMEOUT_SECONDS
        ):
            battery_data = {
                "voltage": battery.voltage,
                "current": battery.current,
                "power": battery.power,
                "suhu_beban": suhu_val,  # Menggunakan suhu_val secara bersih tanpa duplikasi
                "timestamp": battery.timestamp.isoformat(),
            }
            data["BATTERY"] = battery_data
            data["battery"] = battery_data

        # Logika Switching Relay 2 Channel
        plts_v = data["PLTS"]["voltage"] if data["PLTS"] else 0.0
        pltb_v = data["PLTB"]["voltage"] if data["PLTB"] else 0.0

        plts_siap = plts_v >= current_threshold
        pltb_siap = pltb_v >= current_threshold

        if plts_siap and pltb_siap:
            relay_status = 0 if plts_v >= pltb_v else 1
        elif plts_siap:
            relay_status = 0
        elif pltb_siap:
            relay_status = 1
        else:
            relay_status = None

        data["relay"] = relay_status

    return JsonResponse(data)