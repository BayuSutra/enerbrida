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

    timestamp = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.device_id} - {self.system} - {self.timestamp}"