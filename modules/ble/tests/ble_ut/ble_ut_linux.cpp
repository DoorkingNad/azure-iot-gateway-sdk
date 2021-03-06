// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <cstdlib>
#include <cstddef>
#include <cstdbool>
#include "azure_c_shared_utility/macro_utils.h"

#undef DEFINE_ENUM
#define DEFINE_ENUM(enumName, ...) typedef enum C2(enumName, _TAG) { FOR_EACH_1(DEFINE_ENUMERATION_CONSTANT, __VA_ARGS__)} enumName;

#include "testrunnerswitcher.h"
#include "micromock.h"
#include "micromockcharstararenullterminatedstrings.h"

#include <glib.h>
#include <gio/gio.h>

#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/base64.h"
#include "azure_c_shared_utility/vector.h"
#include "azure_c_shared_utility/vector_types_internal.h"
#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/threadapi.h"
#include "message.h"
#include "broker.h"
#include "ble_gatt_io.h"
#include "bleio_seq.h"
#include "messageproperties.h"
#include "ble.h"
#include "module_access.h"

#include <parson.h>

DEFINE_MICROMOCK_ENUM_TO_STRING(MAP_RESULT, MAP_RESULT_VALUES);

static MICROMOCK_MUTEX_HANDLE g_testByTest;
static MICROMOCK_GLOBAL_SEMAPHORE_HANDLE g_dllByDll;

/*these are simple cached variables*/
static pfModule_ParseConfigurationFromJson BLE_ParseConfigurationFromJson = NULL; /*gets assigned in TEST_SUITE_INITIALIZE*/
static pfModule_FreeConfiguration BLE_FreeConfiguration = NULL; /*gets assigned in TEST_SUITE_INITIALIZE*/
static pfModule_Create  BLE_Create = NULL; /*gets assigned in TEST_SUITE_INITIALIZE*/
static pfModule_Destroy BLE_Destroy = NULL; /*gets assigned in TEST_SUITE_INITIALIZE*/
static pfModule_Receive BLE_Receive = NULL; /*gets assigned in TEST_SUITE_INITIALIZE*/

#define FAKE_CONFIG "" \
"{" \
"  \"modules\": [" \
"    {" \
"      \"module name\": \"BLE Printer\"," \
"      \"module path\": \"/ble_printer.so\"," \
"      \"args\": \"\"" \
"    }," \
"    {" \
"      \"module name\": \"SensorTag\"," \
"      \"module path\": \"/ble.so\"," \
"      \"args\": {" \
"        \"controller_index\": 0," \
"        \"device_mac_address\": \"AA:BB:CC:DD:EE:FF\"," \
"        \"instructions\": [" \
"          {" \
"            \"type\": \"read_once\"," \
"            \"characteristic_uuid\": \"00002A24-0000-1000-8000-00805F9B34FB\"" \
"          }," \
"          {" \
"            \"type\": \"write_at_init\"," \
"            \"characteristic_uuid\": \"F000AA02-0451-4000-B000-000000000000\"," \
"            \"data\": \"AQ==\"" \
"          }," \
"          {" \
"            \"type\": \"read_periodic\"," \
"            \"characteristic_uuid\": \"F000AA01-0451-4000-B000-000000000000\"," \
"            \"interval_in_ms\": 1000" \
"          }," \
"          {" \
"            \"type\": \"write_at_exit\"," \
"            \"characteristic_uuid\": \"F000AA02-0451-4000-B000-000000000000\"," \
"            \"data\": \"AA==\"" \
"          }" \
"        ]" \
"      }" \
"    }" \
"  ]" \
"}"

#define GBALLOC_H

extern "C" int   gballoc_init(void);
extern "C" void  gballoc_deinit(void);
extern "C" void* gballoc_malloc(size_t size);
extern "C" void* gballoc_calloc(size_t nmemb, size_t size);
extern "C" void* gballoc_realloc(void* ptr, size_t size);
extern "C" void  gballoc_free(void* ptr);

#ifndef GB_TIME_INTERCEPT
#error these unit tests require the symbol GB_TIME_INTERCEPT to be defined
#else
extern "C"
{
    extern time_t gb_time(time_t *timer);
    extern struct tm* gb_localtime(const time_t *timer);
    extern size_t gb_strftime(char * s, size_t maxsize, const char * format, const struct tm * timeptr);

    typedef union json_value_value {
        char        *string;
        double       number;
        JSON_Object *object;
        JSON_Array  *array;
        int          boolean;
        int          null;
    } JSON_Value_Value;

    struct json_value_t {
        JSON_Value_Type     type;
        JSON_Value_Value    value;
    };

}
#endif

#define TIME_IN_STRFTIME "time"

namespace BASEIMPLEMENTATION
{
    /*if malloc is defined as gballoc_malloc at this moment, there'd be serious trouble*/
#define Lock(x) (LOCK_OK + gballocState - gballocState) /*compiler warning about constant in if condition*/
#define Unlock(x) (LOCK_OK + gballocState - gballocState)
#define Lock_Init() (LOCK_HANDLE)0x42
#define Lock_Deinit(x) (LOCK_OK + gballocState - gballocState)
#include "gballoc.c"
#undef Lock
#undef Unlock
#undef Lock_Init
#undef Lock_Deinit

#include "vector.c"
#include "message.c"
#include "constbuffer.c"
#include "constmap.c"
#include "map.c"
#include "buffer.c"
#include "strings.c"
#include "crt_abstractions.c"

    typedef int JSON_Value_Type;

    typedef union json_value_value {
        char        *string;
        double       number;
        JSON_Object *object;
        JSON_Array  *array;
        int          boolean;
        int          null;
    } JSON_Value_Value;

    struct json_value_t {
        JSON_Value_Type     type;
        JSON_Value_Value    value;
    };

    struct json_object_t {
        char       **names;
        JSON_Value **values;
        size_t       count;
        size_t       capacity;
    };

    typedef struct json_value_t  JSON_Value;
    typedef struct json_object_t JSON_Object;
};

static bool g_call_on_read_complete = false;
static BLEIO_SEQ_RESULT g_read_result = BLEIO_SEQ_OK;

static bool shouldThreadAPI_Create_invoke_callback = false;
static bool should_g_main_loop_quit_call_thread_func = false;
static THREAD_START_FUNC thread_start_func = NULL;
static void* thread_func_arg = NULL;

class CBLEIOSequence
{
public:
    BLEIO_GATT_HANDLE _bleio_gatt_handle;
    VECTOR_HANDLE _instructions;
    ON_BLEIO_SEQ_READ_COMPLETE _on_read_complete;
    ON_BLEIO_SEQ_WRITE_COMPLETE _on_write_complete;

public:
    CBLEIOSequence(
        BLEIO_GATT_HANDLE bleio_gatt_handle,
        VECTOR_HANDLE instructions,
        ON_BLEIO_SEQ_READ_COMPLETE on_read_complete,
        ON_BLEIO_SEQ_WRITE_COMPLETE on_write_complete
    ) :
        _bleio_gatt_handle(bleio_gatt_handle),
        _instructions(instructions),
        _on_read_complete(on_read_complete),
        _on_write_complete(on_write_complete)
    {}

    ~CBLEIOSequence()
    {
        if (_bleio_gatt_handle != NULL)
        {
            BLEIO_gatt_destroy(_bleio_gatt_handle);
        }

        if (_instructions != NULL)
        {
            for (size_t i = 0, len = VECTOR_size(_instructions); i < len; i++)
            {
                BLEIO_SEQ_INSTRUCTION *instruction = (BLEIO_SEQ_INSTRUCTION *)VECTOR_element(_instructions, i);
                if (instruction->characteristic_uuid != NULL)
                {
                    STRING_delete(instruction->characteristic_uuid);
                }
                if (
                    (
                        instruction->instruction_type == WRITE_AT_INIT ||
                        instruction->instruction_type == WRITE_ONCE ||
                        instruction->instruction_type == WRITE_AT_EXIT
                    )
                    &&
                    (
                        instruction->data.buffer != NULL
                    )
                  )
                {
                    BUFFER_delete(instruction->data.buffer);
                }
            }

            VECTOR_destroy(_instructions);
        }
    }

    BLEIO_SEQ_RESULT run()
    {
        size_t len = VECTOR_size(_instructions);
        if (g_call_on_read_complete && _on_read_complete != NULL && len > 0)
        {
            BLEIO_SEQ_INSTRUCTION* instr = (BLEIO_SEQ_INSTRUCTION*)VECTOR_element(_instructions, 0);
            unsigned char fake_data[] = "data";
            size_t data_size = sizeof(fake_data) / sizeof(fake_data[0]);

            _on_read_complete(
                (BLEIO_SEQ_HANDLE)this,
                instr->context,
                STRING_c_str(instr->characteristic_uuid),
                instr->instruction_type,
                g_read_result,
                BUFFER_create(fake_data, data_size)
            );
        }

        return g_read_result;
    }

    BLEIO_SEQ_RESULT add_instruction(BLEIO_SEQ_INSTRUCTION* instruction)
    {
        (void)instruction;
        return BLEIO_SEQ_OK;
    }
};

TYPED_MOCK_CLASS(CBLEMocks, CGlobalMock)
{
public:
    // memory
    MOCK_STATIC_METHOD_1(, void*, gballoc_malloc, size_t, size)
        void* result2 = BASEIMPLEMENTATION::gballoc_malloc(size);
    MOCK_METHOD_END(void*, result2);

    MOCK_STATIC_METHOD_1(, void, gballoc_free, void*, ptr)
        BASEIMPLEMENTATION::gballoc_free(ptr);
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, size_t, VECTOR_size, VECTOR_HANDLE, vector)
        size_t result2 = BASEIMPLEMENTATION::VECTOR_size(vector);
    MOCK_METHOD_END(size_t, result2)

    MOCK_STATIC_METHOD_1(, void, VECTOR_destroy, VECTOR_HANDLE, vector)
        BASEIMPLEMENTATION::VECTOR_destroy(vector);
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, VECTOR_HANDLE, VECTOR_create, size_t, elementSize)
        auto result2 = BASEIMPLEMENTATION::VECTOR_create(elementSize);
    MOCK_METHOD_END(VECTOR_HANDLE, result2)

    MOCK_STATIC_METHOD_2(, void*, VECTOR_element, VECTOR_HANDLE, vector, size_t, index)
        void* result2 = BASEIMPLEMENTATION::VECTOR_element(vector, index);
    MOCK_METHOD_END(void*, result2)

    MOCK_STATIC_METHOD_3(, int, VECTOR_push_back, VECTOR_HANDLE, handle, const void*, elements, size_t, numElements)
        auto result2 = BASEIMPLEMENTATION::VECTOR_push_back(handle, elements, numElements);
    MOCK_METHOD_END(int, result2)

    MOCK_STATIC_METHOD_1(, void*, VECTOR_front, VECTOR_HANDLE, vector)
        void* result2 = BASEIMPLEMENTATION::VECTOR_front(vector);
    MOCK_METHOD_END(void*, result2)

    MOCK_STATIC_METHOD_3(, void, VECTOR_erase, VECTOR_HANDLE, handle, void*, elements, size_t, numElements)
        BASEIMPLEMENTATION::VECTOR_erase(handle, elements, numElements);
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_2(, CONSTBUFFER_HANDLE, CONSTBUFFER_Create, const unsigned char*, source, size_t, size)
        auto result2 = BASEIMPLEMENTATION::CONSTBUFFER_Create(source, size);
    MOCK_METHOD_END(CONSTBUFFER_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, void, CONSTBUFFER_Destroy, CONSTBUFFER_HANDLE, constbufferHandle)
        BASEIMPLEMENTATION::CONSTBUFFER_Destroy(constbufferHandle);
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_1(, CONSTBUFFER_HANDLE, CONSTBUFFER_Clone, CONSTBUFFER_HANDLE, constbufferHandle)
        CONSTBUFFER_HANDLE result2 = BASEIMPLEMENTATION::CONSTBUFFER_Clone(constbufferHandle);
    MOCK_METHOD_END(CONSTBUFFER_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, CONSTBUFFER_HANDLE, CONSTBUFFER_CreateFromBuffer, BUFFER_HANDLE, buffer)
        CONSTBUFFER_HANDLE result2 = BASEIMPLEMENTATION::CONSTBUFFER_CreateFromBuffer(buffer);
    MOCK_METHOD_END(CONSTBUFFER_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, const CONSTBUFFER*, CONSTBUFFER_GetContent, CONSTBUFFER_HANDLE, constbufferHandle)
        const CONSTBUFFER* result2 = BASEIMPLEMENTATION::CONSTBUFFER_GetContent(constbufferHandle);
    MOCK_METHOD_END(const CONSTBUFFER*, result2)
    
    MOCK_STATIC_METHOD_1(, BUFFER_HANDLE, Base64_Decoder, const char*, source)
        auto result2 = BASEIMPLEMENTATION::BUFFER_create((const unsigned char*)"abc", 3);
    MOCK_METHOD_END(BUFFER_HANDLE, result2)

    MOCK_STATIC_METHOD_1(, MESSAGE_HANDLE, Message_Create, const MESSAGE_CONFIG*, cfg)
        MESSAGE_HANDLE result2 = BASEIMPLEMENTATION::Message_Create(cfg);
    MOCK_METHOD_END(MESSAGE_HANDLE, result2)

    MOCK_STATIC_METHOD_1(, MESSAGE_HANDLE, Message_CreateFromBuffer, const MESSAGE_BUFFER_CONFIG*, cfg)
            MESSAGE_HANDLE result1 = BASEIMPLEMENTATION::Message_CreateFromBuffer(cfg);
    MOCK_METHOD_END(MESSAGE_HANDLE, result1)

    MOCK_STATIC_METHOD_1(, MESSAGE_HANDLE, Message_Clone, MESSAGE_HANDLE, message)
        auto result2 = BASEIMPLEMENTATION::Message_Clone(message);
    MOCK_METHOD_END(MESSAGE_HANDLE, result2)

    MOCK_STATIC_METHOD_1(, CONSTMAP_HANDLE, Message_GetProperties, MESSAGE_HANDLE, message)
        CONSTMAP_HANDLE result1 = BASEIMPLEMENTATION::Message_GetProperties(message);
    MOCK_METHOD_END(CONSTMAP_HANDLE, result1)

    MOCK_STATIC_METHOD_1(, const CONSTBUFFER*, Message_GetContent, MESSAGE_HANDLE, message)
        const CONSTBUFFER* result1 = BASEIMPLEMENTATION::Message_GetContent(message);
    MOCK_METHOD_END(const CONSTBUFFER*, result1)

    MOCK_STATIC_METHOD_1(, CONSTBUFFER_HANDLE, Message_GetContentHandle, MESSAGE_HANDLE, message)
        CONSTBUFFER_HANDLE result1 = BASEIMPLEMENTATION::Message_GetContentHandle(message);
    MOCK_METHOD_END(CONSTBUFFER_HANDLE, result1)

    MOCK_STATIC_METHOD_1(, void, Message_Destroy, MESSAGE_HANDLE, message)
        BASEIMPLEMENTATION::Message_Destroy(message);
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_2(, BUFFER_HANDLE, BUFFER_create, const unsigned char*, source, size_t, size)
        BUFFER_HANDLE result2 = BASEIMPLEMENTATION::BUFFER_create(source, size);
    MOCK_METHOD_END(BUFFER_HANDLE, result2)

    MOCK_STATIC_METHOD_1(, void, BUFFER_delete, BUFFER_HANDLE, handle)
        BASEIMPLEMENTATION::BUFFER_delete(handle);
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, BUFFER_HANDLE, BUFFER_clone, BUFFER_HANDLE, handle)
        BUFFER_HANDLE result2 = BASEIMPLEMENTATION::BUFFER_clone(handle);
    MOCK_METHOD_END(BUFFER_HANDLE, result2)

    MOCK_STATIC_METHOD_1(, unsigned char*, BUFFER_u_char, BUFFER_HANDLE, handle)
        auto result2 = BASEIMPLEMENTATION::BUFFER_u_char(handle);
    MOCK_METHOD_END(unsigned char*, result2)

    MOCK_STATIC_METHOD_1(, size_t, BUFFER_length, BUFFER_HANDLE, handle)
        auto result2 = BASEIMPLEMENTATION::BUFFER_length(handle);
    MOCK_METHOD_END(size_t, result2)
    
    MOCK_STATIC_METHOD_1(, void, STRING_delete, STRING_HANDLE, handle)
        BASEIMPLEMENTATION::STRING_delete(handle);
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, const char*, STRING_c_str, STRING_HANDLE, handle)
        const char* result2 = BASEIMPLEMENTATION::STRING_c_str(handle);
    MOCK_METHOD_END(const char*, result2)

    MOCK_STATIC_METHOD_1(, STRING_HANDLE, STRING_clone, STRING_HANDLE, handle)
        auto result2 = BASEIMPLEMENTATION::STRING_clone(handle);
    MOCK_METHOD_END(STRING_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, BLEIO_GATT_HANDLE, BLEIO_gatt_create, const BLE_DEVICE_CONFIG*, config)
        auto result2 = (BLEIO_GATT_HANDLE)malloc(1);
    MOCK_METHOD_END(BLEIO_GATT_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, void, BLEIO_gatt_destroy, BLEIO_GATT_HANDLE, bleio_gatt_handle)
        free(bleio_gatt_handle);
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_3(, int, BLEIO_gatt_connect, BLEIO_GATT_HANDLE, bleio_gatt_handle, ON_BLEIO_GATT_CONNECT_COMPLETE, on_bleio_gatt_connect_complete, void*, callback_context)
        on_bleio_gatt_connect_complete(bleio_gatt_handle, callback_context, BLEIO_GATT_CONNECT_OK);
        int result2 = 0;
    MOCK_METHOD_END(int, result2)
    
    MOCK_STATIC_METHOD_3(, void, BLEIO_gatt_disconnect, BLEIO_GATT_HANDLE, bleio_gatt_handle, ON_BLEIO_GATT_DISCONNECT_COMPLETE, on_bleio_gatt_disconnect_complete, void*, callback_context)
        if (on_bleio_gatt_disconnect_complete != NULL)
        {
            on_bleio_gatt_disconnect_complete(bleio_gatt_handle, callback_context);
        }
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_4(, BLEIO_SEQ_HANDLE, BLEIO_Seq_Create, BLEIO_GATT_HANDLE, bleio_gatt_handle, VECTOR_HANDLE, instructions, ON_BLEIO_SEQ_READ_COMPLETE, on_read_complete, ON_BLEIO_SEQ_WRITE_COMPLETE, on_write_complete)
        auto result2 = (BLEIO_SEQ_HANDLE)new CBLEIOSequence(
            bleio_gatt_handle,
            instructions,
            on_read_complete,
            on_write_complete
        );
    MOCK_METHOD_END(BLEIO_SEQ_HANDLE, result2)
    
    MOCK_STATIC_METHOD_3(, void, BLEIO_Seq_Destroy, BLEIO_SEQ_HANDLE, bleio_seq_handle, ON_BLEIO_SEQ_DESTROY_COMPLETE, on_destroy_complete, void*, context)
        if (on_destroy_complete != NULL)
        {
            on_destroy_complete(bleio_seq_handle, context);
        }
        delete (CBLEIOSequence*)bleio_seq_handle;
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_2(, BLEIO_SEQ_RESULT, BLEIO_Seq_AddInstruction, BLEIO_SEQ_HANDLE, bleio_seq_handle, BLEIO_SEQ_INSTRUCTION*, instruction)
        CBLEIOSequence* seq = (CBLEIOSequence*)bleio_seq_handle;
        auto result2 = seq->add_instruction(instruction);
    MOCK_METHOD_END(BLEIO_SEQ_RESULT, result2)

    MOCK_STATIC_METHOD_1(, BLEIO_SEQ_RESULT, BLEIO_Seq_Run, BLEIO_SEQ_HANDLE, bleio_seq_handle)
        CBLEIOSequence* seq = (CBLEIOSequence*)bleio_seq_handle;
        auto result2 = seq->run();
    MOCK_METHOD_END(BLEIO_SEQ_RESULT, result2)
    
    MOCK_STATIC_METHOD_3(, BROKER_RESULT, Broker_Publish, BROKER_HANDLE, broker, MODULE_HANDLE, source, MESSAGE_HANDLE, message)
        auto result2 = BROKER_OK;
    MOCK_METHOD_END(BROKER_RESULT, result2)
    
    MOCK_STATIC_METHOD_1(, CONSTMAP_HANDLE, ConstMap_Create, MAP_HANDLE, sourceMap)
        auto result2 = BASEIMPLEMENTATION::ConstMap_Create(sourceMap);
    MOCK_METHOD_END(CONSTMAP_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, void, ConstMap_Destroy, CONSTMAP_HANDLE, handle)
        BASEIMPLEMENTATION::ConstMap_Destroy(handle);
    MOCK_VOID_METHOD_END()
    
    MOCK_STATIC_METHOD_1(, MAP_HANDLE, ConstMap_CloneWriteable, CONSTMAP_HANDLE, handle)
        MAP_HANDLE result2 = BASEIMPLEMENTATION::ConstMap_CloneWriteable(handle);
    MOCK_METHOD_END(MAP_HANDLE, result2)
    
    MOCK_STATIC_METHOD_1(, CONSTMAP_HANDLE, ConstMap_Clone, CONSTMAP_HANDLE, handle)
        auto result2 = BASEIMPLEMENTATION::ConstMap_Clone(handle);
    MOCK_METHOD_END(CONSTMAP_HANDLE, result2)
    
    MOCK_STATIC_METHOD_2(, bool, ConstMap_ContainsKey, CONSTMAP_HANDLE, handle, const char*, key)
        auto result2 = BASEIMPLEMENTATION::ConstMap_ContainsKey(handle, key);
    MOCK_METHOD_END(bool, result2)
    
    MOCK_STATIC_METHOD_2(, bool, ConstMap_ContainsValue, CONSTMAP_HANDLE, handle, const char*, value)
        auto result2 = BASEIMPLEMENTATION::ConstMap_ContainsValue(handle, value);
    MOCK_METHOD_END(bool, result2)
    
    MOCK_STATIC_METHOD_2(, const char*, ConstMap_GetValue, CONSTMAP_HANDLE, handle, const char*, key)
        auto result2 = BASEIMPLEMENTATION::ConstMap_GetValue(handle, key);
    MOCK_METHOD_END(const char*, result2)
    
    MOCK_STATIC_METHOD_4(, CONSTMAP_RESULT, ConstMap_GetInternals, CONSTMAP_HANDLE, handle, const char*const**, keys, const char*const**, values, size_t*, count)
        auto result2 = BASEIMPLEMENTATION::ConstMap_GetInternals(handle, keys, values, count);
    MOCK_METHOD_END(CONSTMAP_RESULT, result2)
    
    MOCK_STATIC_METHOD_1(, MAP_HANDLE, Map_Clone, MAP_HANDLE, sourceMap)
        auto result2 = BASEIMPLEMENTATION::Map_Clone(sourceMap);
    MOCK_METHOD_END(MAP_HANDLE, result2);

    MOCK_STATIC_METHOD_1(, void, Map_Destroy, MAP_HANDLE, ptr)
        BASEIMPLEMENTATION::Map_Destroy(ptr);
    MOCK_VOID_METHOD_END();

    MOCK_STATIC_METHOD_3(, MAP_RESULT, Map_ContainsKey, MAP_HANDLE, handle, const char*, key, bool*, keyExists)
        auto result2 = BASEIMPLEMENTATION::Map_ContainsKey(handle, key, keyExists);
    MOCK_METHOD_END(MAP_RESULT, result2);

    MOCK_STATIC_METHOD_3(, MAP_RESULT, Map_ContainsValue, MAP_HANDLE, handle, const char*, value, bool*, valueExists)
        auto result2 = BASEIMPLEMENTATION::Map_ContainsKey(handle, value, valueExists);
    MOCK_METHOD_END(MAP_RESULT, result2);

    MOCK_STATIC_METHOD_2(, const char*, Map_GetValueFromKey, MAP_HANDLE, sourceMap, const char*, key)
        auto result2 = BASEIMPLEMENTATION::Map_GetValueFromKey(sourceMap, key);
    MOCK_METHOD_END(const char*, result2);

    MOCK_STATIC_METHOD_4(, MAP_RESULT, Map_GetInternals, MAP_HANDLE, handle, const char*const**, keys, const char*const**, values, size_t*, count)
        auto result2 = BASEIMPLEMENTATION::Map_GetInternals(handle, keys, values, count);
    MOCK_METHOD_END(MAP_RESULT, result2);
    
    MOCK_STATIC_METHOD_1(, MAP_HANDLE, Map_Create, MAP_FILTER_CALLBACK, mapFilterFunc)
        auto result2 = BASEIMPLEMENTATION::Map_Create(mapFilterFunc);
    MOCK_METHOD_END(MAP_HANDLE, result2)
    
    MOCK_STATIC_METHOD_3(, MAP_RESULT, Map_Add, MAP_HANDLE, handle, const char*, key, const char*, value)
        auto result2 = BASEIMPLEMENTATION::Map_Add(handle, key, value);
    MOCK_METHOD_END(MAP_RESULT, result2)
    
    MOCK_STATIC_METHOD_2(, int, mallocAndStrcpy_s, char**, destination, const char*, source)
        auto result2 = BASEIMPLEMENTATION::mallocAndStrcpy_s(destination, source);
    MOCK_METHOD_END(int, result2);
    
    MOCK_STATIC_METHOD_1(, STRING_HANDLE, STRING_construct, const char*, source)
        auto result2 = BASEIMPLEMENTATION::STRING_construct(source);
    MOCK_METHOD_END(STRING_HANDLE, result2)

    MOCK_STATIC_METHOD_2(, int, STRING_concat, STRING_HANDLE, s1, const char*, s2)
        auto result2 = BASEIMPLEMENTATION::STRING_concat(s1, s2);
    MOCK_METHOD_END(int, result2);
    
    MOCK_STATIC_METHOD_1(, STRING_HANDLE, STRING_new_JSON, const char*, source)
        auto result2 = BASEIMPLEMENTATION::STRING_new_JSON(source);
    MOCK_METHOD_END(STRING_HANDLE, result2)
    
    MOCK_STATIC_METHOD_2(, int, STRING_concat_with_STRING, STRING_HANDLE, s1, STRING_HANDLE, s2)
        auto result2 = BASEIMPLEMENTATION::STRING_concat_with_STRING(s1, s2);
    MOCK_METHOD_END(int, result2);

    MOCK_STATIC_METHOD_3(, THREADAPI_RESULT, ThreadAPI_Create, THREAD_HANDLE*, threadHandle, THREAD_START_FUNC, func, void*, arg)
        THREADAPI_RESULT result2 = THREADAPI_OK;
        thread_start_func = func;
        thread_func_arg = arg;
        if (shouldThreadAPI_Create_invoke_callback == true)
        {
            func(arg);
        }
    MOCK_METHOD_END(THREADAPI_RESULT, result2)

    MOCK_STATIC_METHOD_2(, THREADAPI_RESULT, ThreadAPI_Join, THREAD_HANDLE, threadHandle, int*, res)
        THREADAPI_RESULT result2 = THREADAPI_OK;
    MOCK_METHOD_END(THREADAPI_RESULT, result2)

    MOCK_STATIC_METHOD_1(, time_t, gb_time, time_t *, timer)
        time_t result2 = (time_t)1; /*assume "1" is valid time_t*/
    MOCK_METHOD_END(time_t, result2);

    MOCK_STATIC_METHOD_1(, struct tm*, gb_localtime, const time_t *, timer)
        struct tm* result2 = (struct tm*)0x42;
    MOCK_METHOD_END(struct tm*, result2);

    MOCK_STATIC_METHOD_4(, size_t, gb_strftime, char*, s, size_t, maxsize, const char *, format, const struct tm *, timeptr)
        if (maxsize < strlen(TIME_IN_STRFTIME) + 1)
        {
            ASSERT_FAIL("what is this puny message size!");
        }
        else
        {
            strcpy(s, TIME_IN_STRFTIME);
        }
    MOCK_METHOD_END(size_t, maxsize);

    MOCK_STATIC_METHOD_0(, gint64, g_get_monotonic_time)
        gint64 result2 = 1;
    MOCK_METHOD_END(gint64, result2);

    MOCK_STATIC_METHOD_1(, void, g_usleep, gulong, microseconds)
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_2(, GMainLoop*, g_main_loop_new, GMainContext*, context, gboolean, is_running)
        GMainLoop* result2 = (GMainLoop*)malloc(1);
    MOCK_METHOD_END(GMainLoop*, result2);

    MOCK_STATIC_METHOD_1(, void, g_main_loop_unref, GMainLoop*, loop)
        free(loop);
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, void, g_main_loop_run, GMainLoop*, loop)
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, void, g_main_loop_quit, GMainLoop*, loop)
        if (should_g_main_loop_quit_call_thread_func && thread_start_func != NULL)
        {
            thread_start_func(thread_func_arg);
        }
    MOCK_VOID_METHOD_END()

    MOCK_STATIC_METHOD_1(, gboolean, g_main_loop_is_running, GMainLoop*, loop)
        gboolean result2 = TRUE;
    MOCK_METHOD_END(gboolean, result2);

    MOCK_STATIC_METHOD_1(, GMainContext*, g_main_loop_get_context, GMainLoop*, loop)
        GMainContext* result2 = (GMainContext*)0x42;
    MOCK_METHOD_END(GMainContext*, result2);

    MOCK_STATIC_METHOD_2(, gboolean, g_main_context_iteration, GMainContext*, context, gboolean, may_block)
        gboolean result2 = TRUE;
    MOCK_METHOD_END(gboolean, result2);

    /*Parson Mocks*/
    MOCK_STATIC_METHOD_1(, JSON_Value*, json_parse_string, const char *, filename)
        JSON_Value* value = NULL;
        if (filename != NULL)
        {
            value = (JSON_Value*)malloc(sizeof(JSON_Value));
        }
    MOCK_METHOD_END(JSON_Value*, value);

    MOCK_STATIC_METHOD_1(, JSON_Object*, json_value_get_object, const JSON_Value*, value)
        JSON_Object* object = NULL;
        if (value != NULL)
        {
            object = (JSON_Object*)0x42;
        }
    MOCK_METHOD_END(JSON_Object*, object);

    MOCK_STATIC_METHOD_2(, double, json_object_get_number, const JSON_Object *, object, const char *, name)
        double result2 = 0;
    MOCK_METHOD_END(double, result2);

    MOCK_STATIC_METHOD_2(, const char*, json_object_get_string, const JSON_Object*, object, const char*, name)
        const char* result2;
        if(strcmp(name, "device_mac_address") == 0)
        {
            result2 = "AA:BB:CC:DD:EE:FF";
        }
        else if(strcmp(name, "data") == 0)
        {
            result2 = "AA==";
        }
        else if(strcmp(name, "type") == 0)
        {
            result2 = "read_once";
        }
        else if(strcmp(name, "characteristic_uuid") == 0)
        {
            result2 = "00002A24-0000-1000-8000-00805F9B34FB";
        }
        else
        {
            result2 = NULL;
        }
    MOCK_METHOD_END(const char*, result2);
    
    MOCK_STATIC_METHOD_2(, JSON_Array*, json_object_get_array, const JSON_Object*, object, const char*, name)
        JSON_Array* arr = NULL;
        if (object != NULL && name != NULL)
        {
            arr = (JSON_Array*)0x42;
        }
    MOCK_METHOD_END(JSON_Array*, arr);

    MOCK_STATIC_METHOD_2(, JSON_Object*, json_array_get_object, const JSON_Array*, arr, size_t, index)
        JSON_Object* object = NULL;
        if (arr != NULL)
        {
            object = (JSON_Object*)0x42;
        }
    MOCK_METHOD_END(JSON_Object*, object);

    MOCK_STATIC_METHOD_1(, size_t, json_array_get_count, const JSON_Array*, arr)
        size_t size = 4;
    MOCK_METHOD_END(size_t, size);
    
    MOCK_STATIC_METHOD_1(, void, json_value_free, JSON_Value*, value)
        free(value);
    MOCK_VOID_METHOD_END();
};

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void*, gballoc_malloc, size_t, size);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, gballoc_free, void*, ptr);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , VECTOR_HANDLE, VECTOR_create, size_t, elementSize);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, VECTOR_destroy, VECTOR_HANDLE, vector);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , void*, VECTOR_element, VECTOR_HANDLE, vector, size_t, index);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , size_t, VECTOR_size, VECTOR_HANDLE, vector);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , int, VECTOR_push_back, VECTOR_HANDLE, handle, const void*, elements, size_t, numElements);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void*, VECTOR_front, VECTOR_HANDLE, vector);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , void, VECTOR_erase, VECTOR_HANDLE, handle, void*, elements, size_t, numElements);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , STRING_HANDLE, STRING_construct, const char*, s);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , STRING_HANDLE, STRING_clone, STRING_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , int, STRING_concat, STRING_HANDLE, s1, const char*, s2);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , STRING_HANDLE, STRING_new_JSON, const char*, source);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , int, STRING_concat_with_STRING, STRING_HANDLE, s1, STRING_HANDLE, s2);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , const char*, STRING_c_str, STRING_HANDLE, handle);

DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , CONSTBUFFER_HANDLE, CONSTBUFFER_Create, const unsigned char*, source, size_t, size);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, CONSTBUFFER_Destroy, CONSTBUFFER_HANDLE, constbufferHandle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTBUFFER_HANDLE, CONSTBUFFER_Clone, CONSTBUFFER_HANDLE, constbufferHandle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTBUFFER_HANDLE, CONSTBUFFER_CreateFromBuffer, BUFFER_HANDLE, buffer);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , const CONSTBUFFER*, CONSTBUFFER_GetContent, CONSTBUFFER_HANDLE, constbufferHandle);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , BUFFER_HANDLE, Base64_Decoder, const char*, source);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTMAP_HANDLE, ConstMap_Create, MAP_HANDLE, sourceMap);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, ConstMap_Destroy, CONSTMAP_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MAP_HANDLE, ConstMap_CloneWriteable, CONSTMAP_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTMAP_HANDLE, ConstMap_Clone, CONSTMAP_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , bool, ConstMap_ContainsKey, CONSTMAP_HANDLE, handle, const char*, key);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , bool, ConstMap_ContainsValue, CONSTMAP_HANDLE, handle, const char*, value);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , const char*, ConstMap_GetValue, CONSTMAP_HANDLE, handle, const char*, key);
DECLARE_GLOBAL_MOCK_METHOD_4(CBLEMocks, , CONSTMAP_RESULT, ConstMap_GetInternals, CONSTMAP_HANDLE, handle, const char*const**, keys, const char*const**, values, size_t*, count);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MAP_HANDLE, Map_Create, MAP_FILTER_CALLBACK, mapFilterFunc);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MAP_HANDLE, Map_Clone, MAP_HANDLE, sourceMap);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, Map_Destroy, MAP_HANDLE, ptr);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , MAP_RESULT, Map_ContainsKey, MAP_HANDLE, handle, const char*, key, bool*, keyExists);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , MAP_RESULT, Map_ContainsValue, MAP_HANDLE, handle, const char*, key, bool*, keyExists);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , const char*, Map_GetValueFromKey, MAP_HANDLE, ptr, const char*, key);
DECLARE_GLOBAL_MOCK_METHOD_4(CBLEMocks, , MAP_RESULT, Map_GetInternals, MAP_HANDLE, handle, const char*const**, keys, const char*const**, values, size_t*, count);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , MAP_RESULT, Map_Add, MAP_HANDLE, handle, const char*, key, const char*, value);

DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , int, mallocAndStrcpy_s, char**, destination, const char*, source)

DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , THREADAPI_RESULT, ThreadAPI_Create, THREAD_HANDLE*, threadHandle, THREAD_START_FUNC, func, void*, arg);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , THREADAPI_RESULT, ThreadAPI_Join, THREAD_HANDLE, threadHandle, int*, res);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MESSAGE_HANDLE, Message_Create, const MESSAGE_CONFIG*, cfg);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MESSAGE_HANDLE, Message_CreateFromBuffer, const MESSAGE_BUFFER_CONFIG*, cfg);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , MESSAGE_HANDLE, Message_Clone, MESSAGE_HANDLE, message);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTMAP_HANDLE, Message_GetProperties, MESSAGE_HANDLE, message);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , const CONSTBUFFER*, Message_GetContent, MESSAGE_HANDLE, message);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , CONSTBUFFER_HANDLE, Message_GetContentHandle, MESSAGE_HANDLE, message);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, Message_Destroy, MESSAGE_HANDLE, message);

DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , BUFFER_HANDLE, BUFFER_create, const unsigned char*, source, size_t, size);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, BUFFER_delete, BUFFER_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , unsigned char*, BUFFER_u_char, BUFFER_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , size_t, BUFFER_length, BUFFER_HANDLE, handle);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , BUFFER_HANDLE, BUFFER_clone, BUFFER_HANDLE, handle);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, STRING_delete, STRING_HANDLE, handle);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , BLEIO_GATT_HANDLE, BLEIO_gatt_create, const BLE_DEVICE_CONFIG*, config);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, BLEIO_gatt_destroy, BLEIO_GATT_HANDLE, bleio_gatt_handle);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , int, BLEIO_gatt_connect, BLEIO_GATT_HANDLE, bleio_gatt_handle, ON_BLEIO_GATT_CONNECT_COMPLETE, on_bleio_gatt_connect_complete, void*, callback_context);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , void, BLEIO_gatt_disconnect, BLEIO_GATT_HANDLE, bleio_gatt_handle, ON_BLEIO_GATT_DISCONNECT_COMPLETE, on_bleio_gatt_disconnect_complete, void*, callback_context);

DECLARE_GLOBAL_MOCK_METHOD_4(CBLEMocks, , BLEIO_SEQ_HANDLE, BLEIO_Seq_Create, BLEIO_GATT_HANDLE, bleio_gatt_handle, VECTOR_HANDLE, instructions, ON_BLEIO_SEQ_READ_COMPLETE, on_read_complete, ON_BLEIO_SEQ_WRITE_COMPLETE, on_write_complete);
DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , void, BLEIO_Seq_Destroy, BLEIO_SEQ_HANDLE, bleio_seq_handle, ON_BLEIO_SEQ_DESTROY_COMPLETE, on_destroy_complete, void*, context);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , BLEIO_SEQ_RESULT, BLEIO_Seq_Run, BLEIO_SEQ_HANDLE, bleio_seq_handle);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , BLEIO_SEQ_RESULT, BLEIO_Seq_AddInstruction, BLEIO_SEQ_HANDLE, bleio_seq_handle, BLEIO_SEQ_INSTRUCTION*, instruction);

DECLARE_GLOBAL_MOCK_METHOD_3(CBLEMocks, , BROKER_RESULT, Broker_Publish, BROKER_HANDLE, broker, MODULE_HANDLE, source, MESSAGE_HANDLE, message);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , time_t, gb_time, time_t*, timer);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , struct tm*, gb_localtime, const time_t*, timer);
DECLARE_GLOBAL_MOCK_METHOD_4(CBLEMocks, , size_t, gb_strftime, char*, s, size_t, maxsize, const char *, format, const struct tm *, timeptr);

DECLARE_GLOBAL_MOCK_METHOD_0(CBLEMocks, , gint64, g_get_monotonic_time);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, g_usleep, gulong, microseconds);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , GMainContext*, g_main_loop_get_context, GMainLoop*, loop);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , gboolean, g_main_context_iteration, GMainContext*, context, gboolean, may_block);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , GMainLoop*, g_main_loop_new, GMainContext*, context, gboolean, is_running);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, g_main_loop_unref, GMainLoop*, loop);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, g_main_loop_run, GMainLoop*, loop);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , gboolean, g_main_loop_is_running, GMainLoop*, loop);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, g_main_loop_quit, GMainLoop*, loop);

DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , JSON_Value*, json_parse_string, const char *, filename);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , JSON_Object*, json_value_get_object, const JSON_Value*, value);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , double, json_object_get_number, const JSON_Object*, value, const char*, name);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , const char*, json_object_get_string, const JSON_Object*, object, const char*, name);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , JSON_Array*, json_object_get_array, const JSON_Object*, object, const char*, name);
DECLARE_GLOBAL_MOCK_METHOD_2(CBLEMocks, , JSON_Object*, json_array_get_object, const JSON_Array*, arr, size_t, index);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , size_t, json_array_get_count, const JSON_Array*, arr);
DECLARE_GLOBAL_MOCK_METHOD_1(CBLEMocks, , void, json_value_free, JSON_Value*, value);

BEGIN_TEST_SUITE(ble_ut)
    TEST_SUITE_INITIALIZE(TestClassInitialize)
    {
        TEST_INITIALIZE_MEMORY_DEBUG(g_dllByDll);
        g_testByTest = MicroMockCreateMutex();
        ASSERT_IS_NOT_NULL(g_testByTest);

        const MODULE_API* apis = Module_GetApi(MODULE_API_VERSION_1);

        BLE_ParseConfigurationFromJson = MODULE_PARSE_CONFIGURATION_FROM_JSON(apis);
        BLE_FreeConfiguration = MODULE_FREE_CONFIGURATION(apis);
        BLE_Create = MODULE_CREATE(apis);
        BLE_Destroy = MODULE_DESTROY(apis);
        BLE_Receive = MODULE_RECEIVE(apis);
    }

    TEST_SUITE_CLEANUP(TestClassCleanup)
    {
        MicroMockDestroyMutex(g_testByTest);
        TEST_DEINITIALIZE_MEMORY_DEBUG(g_dllByDll);
    }

    TEST_FUNCTION_INITIALIZE(TestMethodInitialize)
    {
        if (!MicroMockAcquireMutex(g_testByTest))
        {
            ASSERT_FAIL("our mutex is ABANDONED. Failure in test framework");
        }

        g_call_on_read_complete = false;
        g_read_result = BLEIO_SEQ_OK;
        shouldThreadAPI_Create_invoke_callback = false;
        thread_start_func = NULL;
        should_g_main_loop_quit_call_thread_func = false;
    }

    TEST_FUNCTION_CLEANUP(TestMethodCleanup)
    {
        if (!MicroMockReleaseMutex(g_testByTest))
        {
            ASSERT_FAIL("failure in test framework at ReleaseMutex");
        }
    }

    /*Tests_SRS_BLE_05_001: [ BLE_ParseConfigurationFromJson shall return NULL if the configuration parameter is NULL. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_config_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        ///act
        auto result = BLE_ParseConfigurationFromJson(NULL);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_002: [ BLE_ParseConfigurationFromJson shall return NULL if any of the underlying platform calls fail. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_json_parse_string_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((JSON_Value*)NULL);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_003: [ BLE_ParseConfigurationFromJson shall return NULL if the JSON does not start with an object. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_json_value_get_object_for_root_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((JSON_Object*)NULL);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_005: [ BLE_ParseConfigurationFromJson shall return NULL if the controller_index value in the JSON is less than zero. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_controller_index_is_negative)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1)
            .SetFailReturn((int)-1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_004: [ BLE_ParseConfigurationFromJson shall return NULL if there is no device_mac_address property in the JSON. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_mac_address_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1)
            .SetFailReturn((const char*)NULL);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_006: [ BLE_ParseConfigurationFromJson shall return NULL if the instructions array does not exist in the JSON. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instructions_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1)
            .SetFailReturn((JSON_Array *)NULL);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_020: [ BLE_ParseConfigurationFromJson shall return NULL if the instructions array length is equal to zero. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instructions_is_empty)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetReturn((size_t)0);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_002: [ BLE_ParseConfigurationFromJson shall return NULL if any of the underlying platform calls fail. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_VECTOR_create_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1)
            .SetFailReturn((VECTOR_HANDLE)NULL);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_007: [ BLE_ParseConfigurationFromJson shall return NULL if each instruction is not an object. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instruction_entry_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1)
            .SetFailReturn((JSON_Object*)NULL);


        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_008: [ BLE_ParseConfigurationFromJson shall return NULL if a given instruction does not have a type property. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instruction_type_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        
        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetFailReturn((const char*)NULL);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);


        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_009: [ BLE_ParseConfigurationFromJson shall return NULL if a given instruction does not have a characteristic_uuid property. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instruction_characteristic_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1)
            .SetFailReturn((const char*)NULL);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_002: [ BLE_ParseConfigurationFromJson shall return NULL if any of the underlying platform calls fail. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_STRING_construct_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((STRING_HANDLE)NULL);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);


        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_010: [ BLE_ParseConfigurationFromJson shall return NULL if the interval_in_ms value for a read_periodic instruction isn't greater than zero. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_read_periodic_interval_is_zero)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"read_periodic");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "interval_in_ms"))
            .IgnoreArgument(1)
            .SetReturn((uint32_t)0);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_011: [ BLE_ParseConfigurationFromJson shall return NULL if an instruction of type write_at_init or write_at_exit does not have a data property. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_write_instr_data_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1)
            .SetFailReturn((const char*)NULL);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_012: [ BLE_ParseConfigurationFromJson shall return NULL if an instruction of type write_at_init or write_at_exit has a data property whose value does not decode successfully from base 64. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_write_instr_base64_decode_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((BUFFER_HANDLE)NULL);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_021: [ BLE_ParseConfigurationFromJson shall return NULL if a given instruction's type property is unrecognized. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_instr_type_is_unknown)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"booyah_yah");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);


        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_002: [ BLE_ParseConfigurationFromJson shall return NULL if any of the underlying platform calls fail. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_VECTOR_push_back_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2)
            .SetFailReturn((int)__LINE__);

        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_002: [ BLE_ParseConfigurationFromJson shall return NULL if any of the underlying platform calls fail. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_and_frees_first_instr_when_second_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        // cause the second VECTOR_push_back to fail
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2)
            .SetFailReturn((int)__LINE__);

        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_exit");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        // return 2 instructions
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetReturn((size_t)2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_013: [ BLE_ParseConfigurationFromJson shall return NULL if the device_mac_address property's value is not a well-formed MAC address. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_mac_address_is_invalid)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"no mac address here");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetReturn((size_t)1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_05_013: [ BLE_ParseConfigurationFromJson shall return NULL if the device_mac_address property's value is not a well-formed MAC address. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_mac_address_is_invalid2)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Base64_Decoder(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_construct(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_parse_string(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"AA-BB-CC-DD-EE:FF");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1)
            .SetReturn((const char*)"write_at_init");
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "data"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetReturn((size_t)1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_17_001: [ BLE_ParseConfigurationFromJson shall allocate a new BLE_CONFIG structure containing BLE instructions and configuration as parsed from the JSON input. ]*/
    /*Tests_SRS_BLE_05_023: [ BLE_CreateFromJson shall return a non-NULL pointer to the BLE_CONFIG struct allocated if successful. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_succeeds)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(FAKE_CONFIG));
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 2))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 3))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(sizeof(BLE_CONFIG)));

        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_FreeConfiguration(result);
    }

    /*Tests_SRS_BLE_17_001: [ BLE_ParseConfigurationFromJson shall allocate a new BLE_CONFIG structure containing BLE instructions and configuration as parsed from the JSON input. ]*/
    TEST_FUNCTION(BLE_ParseConfigurationFromJson_returns_NULL_when_malloc_fails)
    {
        ///arrange
        CBLEMocks mocks;

        STRICT_EXPECTED_CALL(mocks, json_parse_string(FAKE_CONFIG));
        STRICT_EXPECTED_CALL(mocks, json_value_get_object(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_number(IGNORED_PTR_ARG, "controller_index"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "device_mac_address"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_array(IGNORED_PTR_ARG, "instructions"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_array_get_count(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLE_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 2))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);
        STRICT_EXPECTED_CALL(mocks, json_array_get_object(IGNORED_PTR_ARG, 3))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "type"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, json_object_get_string(IGNORED_PTR_ARG, "characteristic_uuid"))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_construct("00002A24-0000-1000-8000-00805F9B34FB"));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(sizeof(BLE_CONFIG)))
            .SetFailReturn(nullptr);
        STRICT_EXPECTED_CALL(mocks, json_value_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 2))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 3))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);


        ///act
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_17_002: [ BLE_FreeConfiguration shall do nothing if configuration is NULL. ]*/
    /*Tests_SRS_BLE_17_003: [ BLE_FreeConfiguration shall release all resources allocated in the BLE_CONFIG structure and release configuration. ]*/
    TEST_FUNCTION(BLE_FreeConfiguration_does_nothing_with_null)
    {
        CBLEMocks mocks;

        BLE_FreeConfiguration(NULL);

        mocks.AssertActualAndExpectedCalls();
    }

    TEST_FUNCTION(BLE_FreeConfiguration_frees_configuration)
    {
        CBLEMocks mocks;
        auto result = BLE_ParseConfigurationFromJson(FAKE_CONFIG);

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 2))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 3))
            .IgnoreArgument(1); 
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(result));


        BLE_FreeConfiguration(result);
        mocks.AssertActualAndExpectedCalls();
    }

    /*Tests_SRS_BLE_13_001: [ BLE_Create shall return NULL if broker is NULL. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_broker_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        ///act
        auto result = BLE_Create(NULL, (const void*)0x42);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_13_002: [ BLE_Create shall return NULL if configuration is NULL. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_config_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, NULL);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_13_003: [  BLE_Create  shall return  NULL  if  configuration->instructions  is  NULL . ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_config_instructions_is_NULL)
    {
        ///arrange
        CBLEMocks mocks;
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            NULL
        };

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
    }

    /*Tests_SRS_BLE_13_004: [BLE_Create  shall return  NULL  if the  configuration->instructions  vector is empty(size is zero).]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_config_instructions_is_empty)
    {
        ///arrange
        CBLEMocks mocks;
        BLE_CONFIG config =
        {
           { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
           VECTOR_create(sizeof(BLE_INSTRUCTION))
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [  BLE_Create  shall return  NULL  if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_malloc_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((void*)NULL);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        STRING_delete(instr1.characteristic_uuid);
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [  BLE_Create  shall return  NULL  if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_BLEIO_gatt_create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)))
            .SetFailReturn((BLEIO_GATT_HANDLE)NULL);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        STRING_delete(instr1.characteristic_uuid);
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_VECTOR_create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)))
            .SetFailReturn((VECTOR_HANDLE)NULL);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        STRING_delete(instr1.characteristic_uuid);
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_VECTOR_push_back_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
             .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
             .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2)
            .SetFailReturn((int)1);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        STRING_delete(instr1.characteristic_uuid);
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_BLEIO_Seq_Create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetFailReturn((BLEIO_SEQ_HANDLE)NULL);

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
             .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
              .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
              .IgnoreArgument(1);;
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
             .IgnoreArgument(1);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        STRING_delete(instr1.characteristic_uuid);
        VECTOR_destroy(config.instructions);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_g_main_loop_new_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Destroy(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))       // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))          // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))    // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
             .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
             .IgnoreArgument(1);


        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE))
            .SetFailReturn((GMainLoop*)NULL);

        should_g_main_loop_quit_call_thread_func = true;

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        VECTOR_destroy(config.instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_ThreadAPI_Create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Destroy(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))       // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))          // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))    // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetFailReturn((THREADAPI_RESULT)THREADAPI_ERROR);

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
             .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
             .IgnoreArgument(1);


        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, g_main_loop_unref(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        should_g_main_loop_quit_call_thread_func = true;

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        VECTOR_destroy(config.instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_005: [ BLE_Create shall return NULL if an underlying API call fails. ]*/
    /*Tests_SRS_BLE_13_012: [ BLE_Create shall return NULL if BLEIO_gatt_connect returns a non-zero value. ]*/
    TEST_FUNCTION(BLE_Create_returns_NULL_when_BLEIO_gatt_connect_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Destroy(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetFailReturn((int)1);

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))       // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))          // in ~CBLEIOSequence()
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))    // in ~CBLEIOSequence()
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
             .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, g_get_monotonic_time());
        STRICT_EXPECTED_CALL(mocks, g_get_monotonic_time());
        STRICT_EXPECTED_CALL(mocks, g_main_loop_get_context(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_is_running(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_is_running(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_unref(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_quit(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        should_g_main_loop_quit_call_thread_func = true;

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        VECTOR_destroy(config.instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(BLE_Create_returns_NULL_when_BUFFER_clone_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            WRITE_ONCE,
            STRING_construct("fake_char_id"),
            { 0 }
        };
        instr1.data.buffer = BUFFER_create((const unsigned char*)"AQ==", 4);
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_clone(instr1.data.buffer))
            .SetFailReturn((BUFFER_HANDLE)NULL);

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG)) // CBLEIOSequence::run
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
        BUFFER_delete(instr1.data.buffer);
    }

    /*Tests_SRS_BLE_13_006: [ BLE_Create shall return a non-NULL MODULE_HANDLE when successful. ]*/
    /*Tests_SRS_BLE_13_009: [ BLE_Create shall allocate memory for an instance of the BLE_HANDLE_DATA structure and use that as the backing structure for the module handle. ]*/
    /*Tests_SRS_BLE_13_008: [ BLE_Create shall create and initialize the bleio_gatt field in the BLE_HANDLE_DATA object by calling BLEIO_gatt_create. ]*/
    /*Tests_SRS_BLE_13_010: [ BLE_Create shall create and initialize the bleio_seq field in the BLE_HANDLE_DATA object by calling BLEIO_Seq_Create. ]*/
    /*Tests_SRS_BLE_13_011: [ BLE_Create shall asynchronously open a connection to the BLE device by calling BLEIO_gatt_connect. ]*/
    /*Tests_SRS_BLE_13_014: [ If the asynchronous call to BLEIO_gatt_connect is successful then the BLEIO_Seq_Run function shall be called on the bleio_seq field from BLE_HANDLE_DATA. ]*/
    TEST_FUNCTION(BLE_Create_succeeds)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_PERIODIC,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG)) // CBLEIOSequence::run
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_006: [ BLE_Create shall return a non-NULL MODULE_HANDLE when successful. ]*/
    /*Tests_SRS_BLE_13_009: [ BLE_Create shall allocate memory for an instance of the BLE_HANDLE_DATA structure and use that as the backing structure for the module handle. ]*/
    /*Tests_SRS_BLE_13_008: [ BLE_Create shall create and initialize the bleio_gatt field in the BLE_HANDLE_DATA object by calling BLEIO_gatt_create. ]*/
    /*Tests_SRS_BLE_13_010: [ BLE_Create shall create and initialize the bleio_seq field in the BLE_HANDLE_DATA object by calling BLEIO_Seq_Create. ]*/
    /*Tests_SRS_BLE_13_011: [ BLE_Create shall asynchronously open a connection to the BLE device by calling BLEIO_gatt_connect. ]*/
    /*Tests_SRS_BLE_13_014: [ If the asynchronous call to BLEIO_gatt_connect is successful then the BLEIO_Seq_Run function shall be called on the bleio_seq field from BLE_HANDLE_DATA. ]*/
    TEST_FUNCTION(BLE_Create_succeeds_with_write_instruction)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            WRITE_ONCE,
            STRING_construct("fake_char_id"),
            { 0 }
        };
        instr1.data.buffer = BUFFER_create((const unsigned char*)"AQ==", 4);
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_clone(instr1.data.buffer));

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG)) // CBLEIOSequence::run
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
        BUFFER_delete(instr1.data.buffer);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_read_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_ERROR;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_Map_Create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL))
            .SetFailReturn((MAP_HANDLE)NULL);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_time_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL))
            .SetFailReturn((time_t)-1);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_localtime_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((struct tm*)NULL);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_strftime_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetFailReturn((size_t)0);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_first_Map_Add_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3)
            .SetFailReturn((MAP_RESULT)MAP_ERROR);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_second_Map_Add_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3)
            .SetFailReturn((MAP_RESULT)MAP_ERROR);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_third_Map_Add_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_TIMESTAMP_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3)
            .SetFailReturn((MAP_RESULT)MAP_ERROR);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_fourth_Map_Add_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_TIMESTAMP_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_CHARACTERISTIC_UUID_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3)
            .SetFailReturn((MAP_RESULT)MAP_ERROR);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }
    
    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_fifth_Map_Add_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_TIMESTAMP_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_CHARACTERISTIC_UUID_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_TELEMETRY))
            .IgnoreArgument(1)
            .SetFailReturn((MAP_RESULT)MAP_ERROR);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(on_read_complete_does_not_publish_message_when_Message_Create_fails)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_length(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // on_read_complete
        STRICT_EXPECTED_CALL(mocks, BUFFER_u_char(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // on_read_complete

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                 

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_TIMESTAMP_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_CHARACTERISTIC_UUID_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_TELEMETRY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        // mallocAndStrcpy_s is called twice for each property and we have 5 properties
        for (size_t i = 0; i < (5 * 2); i++)
        {
            STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
                .IgnoreAllArguments();
        }

        STRICT_EXPECTED_CALL(mocks, Message_Create(IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .SetFailReturn((MESSAGE_HANDLE)NULL);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_019: [BLE_Create shall handle the ON_BLEIO_SEQ_READ_COMPLETE callback on the BLE I/O sequence. If the call is successful then a new message shall be published on the message broker with the buffer that was read as the content of the message along with the following properties:

    | Property Name           | Description                                                   |
    |-------------------------|---------------------------------------------------------------|
    | ble_controller_index    | The index of the bluetooth radio hardware on the device.      |
    | mac_address             | MAC address of the BLE device from which the data was read.   |
    | timestamp               | Timestamp indicating when the data was read.                  |
    | source                  | This property will always have the value `bleTelemetry`.      |

    ]*/
    TEST_FUNCTION(on_read_complete_publishes_message)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, gballoc_malloc(IGNORED_NUM_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_create(&(config.device_config)));
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_connect(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BUFFER_create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, BUFFER_length(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // on_read_complete
        STRICT_EXPECTED_CALL(mocks, BUFFER_u_char(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // on_read_complete

        STRICT_EXPECTED_CALL(mocks, CONSTBUFFER_Create(IGNORED_PTR_ARG, IGNORED_NUM_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);                                              // on_read_complete
        STRICT_EXPECTED_CALL(mocks, CONSTBUFFER_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, STRING_c_str(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run

        STRICT_EXPECTED_CALL(mocks, VECTOR_create(sizeof(BLEIO_SEQ_INSTRUCTION)));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(instructions, 0));
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);                                              // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(config.instructions));
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                             // CBLEIOSequence::run
        STRICT_EXPECTED_CALL(mocks, STRING_clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);                                              


        STRICT_EXPECTED_CALL(mocks, VECTOR_push_back(IGNORED_PTR_ARG, IGNORED_PTR_ARG, 1))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        STRICT_EXPECTED_CALL(mocks, Map_Create(NULL));
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_BLE_CONTROLLER_INDEX_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_TIMESTAMP_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_CHARACTERISTIC_UUID_PROPERTY, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(3);
        STRICT_EXPECTED_CALL(mocks, Map_Add(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_TELEMETRY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, ConstMap_Create(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, gb_time(NULL));
        STRICT_EXPECTED_CALL(mocks, gb_localtime(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, gb_strftime(IGNORED_PTR_ARG, IGNORED_NUM_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();

        // mallocAndStrcpy_s is called twice for each property and we have 5 properties
        // and we do the whole thing twice
        for (size_t i = 0; i < (5 * 2) * 2; i++)
        {
            STRICT_EXPECTED_CALL(mocks, mallocAndStrcpy_s(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
                .IgnoreAllArguments();
        }

        STRICT_EXPECTED_CALL(mocks, Message_Create(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Message_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Broker_Publish((BROKER_HANDLE)0x42, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreArgument(2)
            .IgnoreArgument(3);

        STRICT_EXPECTED_CALL(mocks, g_main_loop_new(NULL, FALSE));
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Create(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments()
            .SetReturn((THREADAPI_RESULT)THREADAPI_OK);

        ///act
        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(result);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_016: [ If module is NULL BLE_Destroy shall do nothing. ]*/
    TEST_FUNCTION(BLE_Destroy_does_nothing_with_NULL_input)
    {
        ///arrange
        CBLEMocks mocks;

        ///act
        BLE_Destroy(NULL);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
    }

    /*Tests_SRS_BLE_13_017: [ BLE_Destroy shall free all resources. ]*/
    TEST_FUNCTION(BLE_Destroy_happy_path)
    {
        ///arrange
        CBLEMocks mocks;
        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // have the on_read_complete callback called
        g_read_result = BLEIO_SEQ_OK;
        g_call_on_read_complete = true;

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        // we want thread func called from g_main_loop_quit
        should_g_main_loop_quit_call_thread_func = true;

        auto result = BLE_Create((BROKER_HANDLE)0x42, &config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_disconnect(IGNORED_PTR_ARG, NULL, NULL))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, BLEIO_gatt_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_size(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_element(IGNORED_PTR_ARG, 0))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, VECTOR_destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, STRING_delete(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_Destroy(IGNORED_PTR_ARG, IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreAllArguments();
        STRICT_EXPECTED_CALL(mocks, gballoc_free(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_get_monotonic_time());
        STRICT_EXPECTED_CALL(mocks, g_main_loop_get_context(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_run(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_unref(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, g_main_loop_quit(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ThreadAPI_Join(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        ///act
        BLE_Destroy(result);

        ///assert
        mocks.AssertActualAndExpectedCalls();
        ASSERT_IS_NOT_NULL(result);

        ///cleanup
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_26_001: [ `Module_GetApi` shall return a pointer to a `MODULE_API` structure. ]*/
    TEST_FUNCTION(Module_GetApi_returns_non_NULL_and_non_NULL_fields)
    {
        ///arrrange

        ///act
        const MODULE_API* apis = Module_GetApi(MODULE_API_VERSION_1);

        ///assert
        ASSERT_IS_NOT_NULL(apis);
        ASSERT_IS_TRUE(MODULE_PARSE_CONFIGURATION_FROM_JSON(apis) != NULL);
        ASSERT_IS_TRUE(MODULE_FREE_CONFIGURATION(apis) != NULL);
        ASSERT_IS_TRUE(MODULE_CREATE(apis) != NULL);
        ASSERT_IS_TRUE(MODULE_DESTROY(apis) != NULL);
        ASSERT_IS_TRUE(MODULE_RECEIVE(apis) != NULL);
    }

    /*Tests_SRS_BLE_13_018: [ BLE_Receive shall do nothing if module is NULL or if message is NULL. ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_module_handle_is_NULL)
    {
        ///arrrange
        CBLEMocks mocks;

        ///act
        BLE_Receive(NULL, (MESSAGE_HANDLE)0x42);

        ///assert
        mocks.AssertActualAndExpectedCalls();
    }

    /*Tests_SRS_BLE_13_018: [ BLE_Receive shall do nothing if module is NULL or if message is NULL. ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_is_NULL)
    {
        ///arrrange
        CBLEMocks mocks;

        ///act
        BLE_Receive((MODULE_HANDLE)0x42, NULL);

        ///assert
        mocks.AssertActualAndExpectedCalls();
    }

    /*Tests_SRS_BLE_13_020: [ BLE_Receive shall ignore all messages except those that have the following properties:
        >| Property Name           | Description                                                             |
        >|-------------------------|-------------------------------------------------------------------------|
        >| source                  | This property should have the value "BLE".                              |
        >| macAddress              | MAC address of the BLE device to which the data to should be written.   |
    ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_does_not_contain_source_property)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL); // empty map
        MESSAGE_CONFIG message_config =
        {
            0, NULL,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_020: [ BLE_Receive shall ignore all messages except those that have the following properties:
        >| Property Name           | Description                                                             |
        >|-------------------------|-------------------------------------------------------------------------|
        >| source                  | This property should have the value "BLE".                              |
        >| macAddress              | MAC address of the BLE device to which the data to should be written.   |
    ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_contains_source_property_with_invalid_module)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, "boo");
        MESSAGE_CONFIG message_config =
        {
            0, NULL,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
    STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_020: [ BLE_Receive shall ignore all messages except those that have the following properties:
        >| Property Name           | Description                                                             |
        >|-------------------------|-------------------------------------------------------------------------|
        >| source                  | This property should have the value "BLE".                              |
        >| macAddress              | MAC address of the BLE device to which the data to should be written.   |
    ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_does_not_contain_mac_address)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_COMMAND);
        MESSAGE_CONFIG message_config =
        {
            0, NULL,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
    STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_020: [ BLE_Receive shall ignore all messages except those that have the following properties:
        >| Property Name           | Description                                                             |
        >|-------------------------|-------------------------------------------------------------------------|
        >| source                  | This property should have the value "BLE".                              |
        >| macAddress              | MAC address of the BLE device to which the data to should be written.   |
    ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_contains_invalid_mac_address)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_COMMAND);
        Map_Add(properties, GW_MAC_ADDRESS_PROPERTY, "boo");

        MESSAGE_CONFIG message_config =
        {
            0, NULL,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
    STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_022: [ BLE_Receive shall ignore the message unless the 'macAddress' property matches the MAC address that was passed to this module when it was created. ]*/
    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_contains_foreign_mac_address)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_COMMAND);
        Map_Add(properties, GW_MAC_ADDRESS_PROPERTY, "FF:EE:DD:CC:BB:AA");

        MESSAGE_CONFIG message_config =
        {
            0, NULL,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
    STRING_delete(instr1.characteristic_uuid);
    }

    TEST_FUNCTION(BLE_Receive_does_nothing_when_message_does_not_have_content)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_COMMAND);
        Map_Add(properties, GW_MAC_ADDRESS_PROPERTY, "AA:BB:CC:DD:EE:FF");

        MESSAGE_CONFIG message_config =
        {
            0, NULL, // no content
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));
        STRICT_EXPECTED_CALL(mocks, Message_GetContent(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, CONSTBUFFER_GetContent(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
    }

    /*Tests_SRS_BLE_13_021: [ BLE_Receive shall treat the content of the message as a BLE_INSTRUCTION and schedule it for execution by calling BLEIO_Seq_AddInstruction. ]*/
    TEST_FUNCTION(BLE_Receive_calls_BLEIO_Seq_AddInstruction)
    {
        ///arrrange
        CBLEMocks mocks;

        VECTOR_HANDLE instructions = VECTOR_create(sizeof(BLE_INSTRUCTION));
        BLE_INSTRUCTION instr1 =
        {
            READ_ONCE,
            STRING_construct("fake_char_id"),
            { 500 }
        };
        VECTOR_push_back(instructions, &instr1, 1);
        BLE_CONFIG config =
        {
            { { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }, 0 },
            instructions
        };

        // we don't want ThreadAPI_Create to call the callback
        shouldThreadAPI_Create_invoke_callback = false;

        auto handle = BLE_Create((BROKER_HANDLE)0x42, &config);

        MAP_HANDLE properties = Map_Create(NULL);
        Map_Add(properties, GW_SOURCE_PROPERTY, GW_SOURCE_BLE_COMMAND);
        Map_Add(properties, GW_MAC_ADDRESS_PROPERTY, "AA:BB:CC:DD:EE:FF");

        BLE_INSTRUCTION instruction =
        {
            WRITE_ONCE,
            STRING_construct("fake_char_id"),
            { .buffer = BUFFER_create((const unsigned char*)"data", 4) }
        };

        MESSAGE_CONFIG message_config =
        {
            sizeof(BLE_INSTRUCTION),
            (const unsigned char*)&instruction,
            properties
        };

        MESSAGE_HANDLE message = Message_Create(&message_config);
        mocks.ResetAllCalls();

        STRICT_EXPECTED_CALL(mocks, Message_GetProperties(message));
        STRICT_EXPECTED_CALL(mocks, Message_GetContent(message));

        STRICT_EXPECTED_CALL(mocks, ConstMap_Clone(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_Destroy(IGNORED_PTR_ARG))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, ConstMap_GetValue(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, CONSTBUFFER_GetContent(IGNORED_PTR_ARG))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_SOURCE_PROPERTY))
            .IgnoreArgument(1);
        STRICT_EXPECTED_CALL(mocks, Map_GetValueFromKey(IGNORED_PTR_ARG, GW_MAC_ADDRESS_PROPERTY))
            .IgnoreArgument(1);

        STRICT_EXPECTED_CALL(mocks, BLEIO_Seq_AddInstruction(IGNORED_PTR_ARG, IGNORED_PTR_ARG))
            .IgnoreArgument(1)
            .IgnoreArgument(2);

        ///act
        BLE_Receive(handle, message);

        ///assert
        mocks.AssertActualAndExpectedCalls();

        ///cleanup
        should_g_main_loop_quit_call_thread_func = true;
        BLE_Destroy(handle);
        Map_Destroy(properties);
        Message_Destroy(message);
        VECTOR_destroy(instructions);
        STRING_delete(instr1.characteristic_uuid);
        BUFFER_delete(instruction.data.buffer);
        STRING_delete(instruction.characteristic_uuid);
    }

END_TEST_SUITE(ble_ut)
