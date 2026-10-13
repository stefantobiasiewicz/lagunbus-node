/* GENERATED FILE — do not edit it by hand.
 *
 * Source:    the .yaml files in lagunbus-spec/registry/
 * Generator: tools/generate_registry.py
 *
 * A change in the spec registry breaks the build until this file is generated
 * anew. That is what it is here for.
 */
#ifndef LAGUNBUS_REGISTRY_H
#define LAGUNBUS_REGISTRY_H

#include <stdint.h>

/* Message kinds — the KIND field. THE NUMBER IS THE PRIORITY (05-mapowanie-can.md §2). */
#define LB_KIND_ALARM                      0x0u
#define LB_KIND_EVENT                      0x1u
#define LB_KIND_COMMAND                    0x3u
#define LB_KIND_GROUP_COMMAND              0x4u
#define LB_KIND_WRITE                      0x5u
#define LB_KIND_READ                       0x6u
#define LB_KIND_RESPONSE                   0x7u
#define LB_KIND_REPORT                     0x8u
#define LB_KIND_LONG_RESPONSE              0x9u
#define LB_KIND_LONG_WRITE                 0xAu
#define LB_KIND_QUERY                      0xBu
#define LB_KIND_HUB_RESPONSE               0xCu
#define LB_KIND_JOIN                       0xFu

/* Result codes of a reply, the RESULT field, 5 bits (04-interakcje.md §8). */
#define LB_RESULT_OK                       0x00u
#define LB_RESULT_UNSUPPORTED_ENDPOINT     0x01u
#define LB_RESULT_UNSUPPORTED_ELEMENT      0x02u
#define LB_RESULT_UNSUPPORTED_FEATURE      0x03u
#define LB_RESULT_READ_ONLY                0x04u
#define LB_RESULT_WRITE_ONLY               0x05u
#define LB_RESULT_INVALID_VALUE            0x06u
#define LB_RESULT_INVALID_STATE            0x07u
#define LB_RESULT_BUSY                     0x08u
#define LB_RESULT_FAILURE                  0x09u
#define LB_RESULT_UNAVAILABLE              0x0Cu
#define LB_RESULT_SEGMENTED                0x0Bu
#define LB_RESULT_INSUFFICIENT_SPACE       0x0Au

/* Node classes — the NodeClass field in an ANNOUNCEMENT. Chooses the address block (03-adresacja.md §2.3). */
#define LB_CLASS_UNSPECIFIED               0x00u
#define LB_CLASS_CONTROL                   0x01u
#define LB_CLASS_SENSOR                    0x02u

/* Endpoint types (registry/endpoint-types.yaml). */
#define LB_TYPE_DEVICE                     0x0001u
#define LB_TYPE_DIAGNOSTICS                0x0002u
#define LB_TYPE_SCENES                     0x0003u
#define LB_TYPE_GROUPS                     0x0004u
#define LB_TYPE_KEYINPUT                   0x0010u
#define LB_TYPE_VALUEINPUT                 0x0011u
#define LB_TYPE_CONTACT                    0x0012u
#define LB_TYPE_TOGGLEINPUT                0x0013u
#define LB_TYPE_OUTPUT                     0x0020u
#define LB_TYPE_COVER                      0x0021u
#define LB_TYPE_DISPLAY                    0x0022u
#define LB_TYPE_SENSE                      0x0030u
#define LB_TYPE_THERMOSTAT                 0x0040u
#define LB_TYPE_METER                      0x0031u

/* Common attributes, range 0xF0-0xFF (01-model-danych.md §4). */
#define LB_COMMON_TYPEID                   0xF0u
#define LB_COMMON_TYPEREVISION             0xF1u
#define LB_COMMON_FEATURES                 0xF2u
#define LB_COMMON_ENABLED                  0xF3u
#define LB_COMMON_CHANNEL                  0xF5u
#define LB_COMMON_QUERYENABLED             0xF6u
#define LB_COMMON_REPORTINTERVAL           0xF8u
#define LB_COMMON_REPORTMINGAP             0xF9u
#define LB_COMMON_REPORTHYSTERESIS         0xFAu

/* Name of a message kind — for watching traffic, not for logic. */
static inline const char *lb_kind_name(uint8_t num)
{
    switch (num) {
    case 0x00: return "ALARM";
    case 0x01: return "EVENT";
    case 0x03: return "COMMAND";
    case 0x04: return "GROUP_COMMAND";
    case 0x05: return "WRITE";
    case 0x06: return "READ";
    case 0x07: return "RESPONSE";
    case 0x08: return "REPORT";
    case 0x09: return "LONG_RESPONSE";
    case 0x0A: return "LONG_WRITE";
    case 0x0B: return "QUERY";
    case 0x0C: return "HUB_RESPONSE";
    case 0x0F: return "JOIN";
    default: return "?";
    }
}

/* Name of a result code. `?` means a code from outside the registry — and that has to be shown too. */
static inline const char *lb_result_name(uint8_t num)
{
    switch (num) {
    case 0x00: return "OK";
    case 0x01: return "UNSUPPORTED_ENDPOINT";
    case 0x02: return "UNSUPPORTED_ELEMENT";
    case 0x03: return "UNSUPPORTED_FEATURE";
    case 0x04: return "READ_ONLY";
    case 0x05: return "WRITE_ONLY";
    case 0x06: return "INVALID_VALUE";
    case 0x07: return "INVALID_STATE";
    case 0x08: return "BUSY";
    case 0x09: return "FAILURE";
    case 0x0C: return "UNAVAILABLE";
    case 0x0B: return "SEGMENTED";
    case 0x0A: return "INSUFFICIENT_SPACE";
    default: return "?";
    }
}

#endif /* LAGUNBUS_REGISTRY_H */
