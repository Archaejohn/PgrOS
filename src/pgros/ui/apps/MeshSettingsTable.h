#pragma once
// GENERATED FILE -- do not edit by hand.
//
// Produced by scripts/gen_mesh_settings.js from
// vendor/firmware/protobufs/meshtastic/config.proto.
// Regenerate after moving the submodule pin:
//
//     node scripts/gen_mesh_settings.js
//
// Only the enum vocabularies are generated. Which settings PgrOS surfaces is a
// curated decision and lives in SettingsApp.cpp -- the phone app is still the
// right place to configure a node, and most of the config surface has no
// business on a 480x222 screen driven by a rotary.

#include <stdint.h>

namespace pgros {

struct MeshEnumValue {
    int32_t value;
    const char *label;
};

// meshtastic Role (11 values)
static const MeshEnumValue kRoleValues[] = {
    {meshtastic_Config_DeviceConfig_Role_CLIENT, "Client"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE, "Client Mute"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER, "Router"},
    {meshtastic_Config_DeviceConfig_Role_TRACKER, "Tracker"},
    {meshtastic_Config_DeviceConfig_Role_SENSOR, "Sensor"},
    {meshtastic_Config_DeviceConfig_Role_TAK, "TAK"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN, "Client Hidden"},
    {meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND, "Lost And Found"},
    {meshtastic_Config_DeviceConfig_Role_TAK_TRACKER, "TAK Tracker"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER_LATE, "Router Late"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_BASE, "Client Base"},
};
static const uint8_t kRoleValuesCount = 11;

// meshtastic RegionCode (37 values)
static const MeshEnumValue kRegionValues[] = {
    {meshtastic_Config_LoRaConfig_RegionCode_UNSET, "Unset"},
    {meshtastic_Config_LoRaConfig_RegionCode_US, "US"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_433, "EU 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_868, "EU 868"},
    {meshtastic_Config_LoRaConfig_RegionCode_CN, "CN"},
    {meshtastic_Config_LoRaConfig_RegionCode_JP, "JP"},
    {meshtastic_Config_LoRaConfig_RegionCode_ANZ, "ANZ"},
    {meshtastic_Config_LoRaConfig_RegionCode_KR, "KR"},
    {meshtastic_Config_LoRaConfig_RegionCode_TW, "TW"},
    {meshtastic_Config_LoRaConfig_RegionCode_RU, "RU"},
    {meshtastic_Config_LoRaConfig_RegionCode_IN, "IN"},
    {meshtastic_Config_LoRaConfig_RegionCode_NZ_865, "NZ 865"},
    {meshtastic_Config_LoRaConfig_RegionCode_TH, "TH"},
    {meshtastic_Config_LoRaConfig_RegionCode_LORA_24, "LoRa 24"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_433, "UA 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_433, "MY 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_919, "MY 919"},
    {meshtastic_Config_LoRaConfig_RegionCode_SG_923, "SG 923"},
    {meshtastic_Config_LoRaConfig_RegionCode_PH_433, "PH 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_PH_868, "PH 868"},
    {meshtastic_Config_LoRaConfig_RegionCode_PH_915, "PH 915"},
    {meshtastic_Config_LoRaConfig_RegionCode_ANZ_433, "ANZ 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_KZ_433, "KZ 433"},
    {meshtastic_Config_LoRaConfig_RegionCode_KZ_863, "KZ 863"},
    {meshtastic_Config_LoRaConfig_RegionCode_NP_865, "NP 865"},
    {meshtastic_Config_LoRaConfig_RegionCode_BR_902, "BR 902"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU1_2M, "Itu1 2m"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU2_2M, "Itu2 2m"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_866, "EU 866"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_874, "EU 874"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_917, "EU 917"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_N_868, "EU N 868"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU3_2M, "Itu3 2m"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU1_70CM, "Itu1 70cm"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU2_70CM, "Itu2 70cm"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU3_70CM, "Itu3 70cm"},
    {meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM, "Itu2 125cm"},
};
static const uint8_t kRegionValuesCount = 37;

// meshtastic ModemPreset (15 values)
static const MeshEnumValue kPresetValues[] = {
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, "Long Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, "Medium Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST, "Medium Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, "Short Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST, "Short Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE, "Long Moderate"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, "Short Turbo"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO, "Long Turbo"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LITE_FAST, "Lite Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LITE_SLOW, "Lite Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST, "Narrow Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW, "Narrow Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST, "Tiny Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_TINY_SLOW, "Tiny Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO, "Medium Turbo"},
};
static const uint8_t kPresetValuesCount = 15;

// meshtastic PairingMode (3 values)
static const MeshEnumValue kPairingValues[] = {
    {meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN, "Random PIN"},
    {meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN, "Fixed PIN"},
    {meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN, "No PIN"},
};
static const uint8_t kPairingValuesCount = 3;

} // namespace pgros
