from django.db import models


class SensorData(models.Model):

    SYSTEM_CHOICES = [
        ('PLTS', 'PLTS'),
        ('PLTB', 'PLTB'),
        ('BATTERY', 'BATTERY'),
    ]

    device_id = models.CharField(max_length=100)
    system = models.CharField(
        max_length=10,
        choices=SYSTEM_CHOICES
    )

    voltage = models.FloatField(default=0)
    current = models.FloatField(default=0)
    power = models.FloatField(default=0)
    soc = models.FloatField(default=0, null=True, blank=True)
    
    suhu_beban = models.FloatField(null=True, blank=True)

    timestamp = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.device_id} - {self.system} - {self.timestamp}"


class SystemSetting(models.Model):
    key = models.CharField(max_length=50, unique=True)
    value = models.CharField(max_length=100)
    description = models.CharField(max_length=200, blank=True, null=True)
    updated_at = models.DateTimeField(auto_now=True)

    def __str__(self):
        return f"{self.key}: {self.value}"


class SystemLog(models.Model):
    CATEGORY_CHOICES = [
        ('MQTT', 'MQTT Log'),
        ('WEB', 'Web / HTTP Log'),
        ('DATABASE', 'Database Log'),
        ('DOWNLOAD', 'Download CSV Log'),
        ('SYSTEM', 'System Log'),
    ]

    LEVEL_CHOICES = [
        ('INFO', 'INFO'),
        ('SUCCESS', 'SUCCESS'),
        ('WARNING', 'WARNING'),
        ('ERROR', 'ERROR'),
    ]

    category = models.CharField(max_length=20, choices=CATEGORY_CHOICES, db_index=True)
    level = models.CharField(max_length=10, choices=LEVEL_CHOICES, default='INFO')
    message = models.TextField()
    metadata = models.JSONField(blank=True, null=True)
    created_at = models.DateTimeField(auto_now_add=True, db_index=True)

    class Meta:
        ordering = ['-created_at']

    def __str__(self):
        return f"[{self.created_at.strftime('%Y-%m-%d %H:%M:%S')}] [{self.category}] [{self.level}] {self.message[:60]}"
