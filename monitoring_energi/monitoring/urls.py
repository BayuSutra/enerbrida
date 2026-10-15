from django.urls import path

from . import views


urlpatterns = [

    path(
        "",
        views.dashboard,
        name="dashboard"
    ),

    path(
        "config/",
        views.config_view,
        name="config"
    ),

    path(
        "api/latest/",
        views.latest_data,
        name="latest_data"
    ),

    path(
        "api/set-threshold/",
        views.set_threshold,
        name="set_threshold"
    ),

    path(
        "api/set-load-name/",
        views.set_load_name,
        name="set_load_name"
    ),

    path(
        "api/logs/",
        views.api_logs,
        name="api_logs"
    ),

    path(
        "api/clear-logs/",
        views.api_clear_logs,
        name="api_clear_logs"
    ),

    path(
        "api/ota-update/",
        views.trigger_ota,
        name="trigger_ota"
    ),

    path(
        "export/csv/",
        views.export_csv,
        name="export_csv"
    ),
]