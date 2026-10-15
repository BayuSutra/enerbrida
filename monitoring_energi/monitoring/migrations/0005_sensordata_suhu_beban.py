from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("monitoring", "0004_systemlog"),
    ]

    operations = [
        migrations.AddField(
            model_name="sensordata",
            name="suhu_beban",
            field=models.FloatField(blank=True, null=True),
        ),
    ]
