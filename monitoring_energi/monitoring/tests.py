import json

from django.test import TestCase

from .models import SensorData, SystemSetting


class LoadTemperatureApiTests(TestCase):
    def test_config_page_sets_csrf_cookie_and_current_load_name(self):
        SystemSetting.objects.create(key="load_name", value="Pompa Air")

        response = self.client.get("/config/")

        self.assertEqual(response.status_code, 200)
        self.assertIn("csrftoken", self.client.cookies)
        self.assertContains(response, 'value="Pompa Air"')

    def test_latest_data_includes_temperature_and_saved_load_name(self):
        SystemSetting.objects.create(key="load_name", value="Pompa Air")
        SensorData.objects.create(
            device_id="ESP32-001",
            system="PLTS",
            voltage=24,
            current=1,
            power=24,
            suhu_beban=31.5,
        )

        response = self.client.get("/api/latest/")

        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertEqual(payload["load_name"], "Pompa Air")
        self.assertEqual(payload["PLTS"]["suhu_beban"], 31.5)

    def test_set_load_name_saves_trimmed_name(self):
        response = self.client.post(
            "/api/set-load-name/",
            data=json.dumps({"load_name": "  Pompa Air  "}),
            content_type="application/json",
        )

        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["load_name"], "Pompa Air")
        self.assertEqual(
            SystemSetting.objects.get(key="load_name").value,
            "Pompa Air",
        )

    def test_set_load_name_rejects_blank_name(self):
        response = self.client.post(
            "/api/set-load-name/",
            data=json.dumps({"load_name": "   "}),
            content_type="application/json",
        )

        self.assertEqual(response.status_code, 400)
