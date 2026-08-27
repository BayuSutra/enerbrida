from django.contrib import admin

# Register your models here.
from django.contrib import admin
from .models import SensorData


@admin.register(SensorData)
class SensorDataAdmin(admin.ModelAdmin):

    list_display = (
        'device_id',
        'system',
        'voltage',
        'current',
        'power',
        'timestamp',
    )

    list_filter = (
        'system',
        'device_id',
    )

    ordering = (
        '-timestamp',
    )