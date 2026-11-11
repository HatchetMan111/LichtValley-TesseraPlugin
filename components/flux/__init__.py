"""LichtValley Flux: solar, battery, house and grid on a tile, and a card with the day so far.

Based on hflocki/tessera-energy (MIT), extended with the battery's power and state of charge.

plugin.yaml reads every value from Home Assistant as a text sensor; this component hands them to the C++ by the index
of FIELDS, which flux.h numbers the same way. register_plugin() takes the id, version, tile memory and the
screen's texts from the manifest and translations/ beside this folder.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["smart_display"]
AUTO_LOAD = ["text_sensor"]

# The order is the C++'s (enum Field in flux.h); never reorder one without the other.
FIELDS = ['grid', 'grid_unit', 'pv', 'pv_unit', 'pv_today', 'pv_today_unit', 'coverage', 'forecast', 'forecast_unit',
          'grid_today', 'grid_today_unit', 'bat_rate', 'bat_rate_unit', 'soc']

flux_ns = cg.esphome_ns.namespace("flux")
Flux = flux_ns.class_("Flux", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {cv.GenerateID(): cv.declare_id(Flux)}
    | {cv.Optional(name): cv.use_id(text_sensor.TextSensor) for name in FIELDS}
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    for index, name in enumerate(FIELDS):
        if name in config:
            cg.add(var.set_field(index, await cg.get_variable(config[name])))
