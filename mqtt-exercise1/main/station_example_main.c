/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdlib.h> // Necesario para strdup y free
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
// #include "cJSON.h" // Se ha eliminado la dependencia de cJSON

#include "lwip/err.h"
#include "lwip/sys.h"
#include "mqtt_client.h"
#include "led_strip.h"

// --- CONFIGURACIÓN DE HARDWARE ---
#define LED_STRIP_GPIO    GPIO_NUM_2
#define LED_NUMBERS       1

// --- CONFIGURACIÓN DE RED Y MQTT ---
#define EXAMPLE_ESP_WIFI_SSID      "Javastral" // Cambia tu SSID
#define EXAMPLE_ESP_WIFI_PASS      "damedane"  // Cambia tu Contraseña
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#define MQTT_BROKER_URI "mqtt://18.218.20.155:1883" // IP del broker Mosquitto (Puerto 1883)
#define MQTT_USER       "esp32"
#define MQTT_PASS       "13310625"

#define CONFIG_TOPIC "esp32/config" // Topic para recibir la configuración de la web
#define MONITOR_TOPIC "esp32/led"    // Topic para publicar el color actual

// --- VARIABLES GLOBALES DE ESTADO ---
static esp_mqtt_client_handle_t mqtt_client = NULL;
static led_strip_handle_t led_strip = NULL;

// Variables de estado dinámicas
static int current_interval_ms = 3000;
static char current_colors[100] = "red,green,blue"; 
static TaskHandle_t led_task_handle = NULL; 

static const char *TAG = "MAIN";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;


// --- FUNCIONES DE UTILIDAD DEL LED ---

static void led_set_color(const char* color) {
    uint8_t r = 0, g = 0, b = 0;
    
    // Mapeo simple de nombres a valores GRB para WS2812
    if (strcmp(color, "red") == 0) {
        r = 255;
    } else if (strcmp(color, "green") == 0) {
        g = 255;
    } else if (strcmp(color, "blue") == 0) {
        b = 255;
    } else {
        ESP_LOGW("LED", "Color '%s' no reconocido. Apagando.", color);
    }
    
    // El WS2812 en este caso usa formato GRB, no RGB
    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, g, r, b)); 
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    
    ESP_LOGI("LED", "Encendiendo LED color: %s (R:%d, G:%d, B:%d)", color, r, g, b);
}

// --- FUNCIONES DE UTILIDAD PARA EL PARSEO DE COLORES ---

// Devuelve una copia del nombre del color en la posición 'index'. Debe ser liberada con 'free()'.
char* get_color_at_index(const char* color_list, int index) {
    // Es CRUCIAL trabajar con una copia de la lista de colores, ya que strtok la modifica.
    char temp_list[100];
    strncpy(temp_list, color_list, 100);
    temp_list[99] = '\0';
    
    // Inicializar strtok para la primera llamada.
    char *token = strtok(temp_list, ",");
    int i = 0;
    int total_colors = 0;
    
    // Contar cuántos colores hay para el módulo (ciclo)
    while (token != NULL) {
        total_colors++;
        if (i == index) {
            return strdup(token); 
        }
        token = strtok(NULL, ",");
        i++;
    }
    
    // Lógica Cíclica: Si el índice es mayor al número de colores, reiniciamos.
    if (total_colors > 0) {
        // Reiniciamos la búsqueda para el índice modular
        return get_color_at_index(color_list, index % total_colors);
    }
    
    // Valor por defecto si la lista está vacía/malformada
    return strdup("red"); 
}

// --- TAREA LED DE CONTROL REMOTO ---

static void led_task(void *pvParameters) {
    int color_index = 0;
    ESP_LOGI("LED_TASK", "Tarea de LED iniciada.");
    while (1) {
        // 1. Obtener el color actual de la secuencia (maneja el ciclo internamente)
        char* color_name = get_color_at_index(current_colors, color_index);

        if (color_name) {
            // 2. Encender el LED
            led_set_color(color_name);
            
            // 3. Publicar el estado actual en el topic de monitoreo
            if (mqtt_client) {
                esp_mqtt_client_publish(mqtt_client, MONITOR_TOPIC, color_name, 0, 1, 0);
            }
            
            ESP_LOGI("LED_TASK", "Color: %s. Próximo cambio en %d ms", color_name, current_interval_ms);

            // 4. Liberar la memoria del nombre del color (asignada por strdup)
            free(color_name); 
        }

        // 5. Espera el intervalo definido remotamente
        vTaskDelay(pdMS_TO_TICKS(current_interval_ms));
        
        // 6. Apaga el LED brevemente y avanza al siguiente color
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 0, 0)); 
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Breve pausa

        color_index++;
    }
}

// --- MANEJADOR DE EVENTOS MQTT MODIFICADO (SIN cJSON) ---

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Connected. Subscribing to %s", CONFIG_TOPIC);
            // Suscripción al topic de configuración (¡CRUCIAL!)
            esp_mqtt_client_subscribe(event->client, CONFIG_TOPIC, 0); 
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI("MQTT", "Disconnected.");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI("MQTT", "Data received. TOPIC=%.*s DATA=%.*s", event->topic_len, event->topic, event->data_len, event->data);
            
            if (strncmp(event->topic, CONFIG_TOPIC, event->topic_len) == 0) {
                
                // Copiamos el payload y le añadimos un null-terminator para usar funciones de string estándar
                char payload[event->data_len + 1];
                strncpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';
                
                int new_interval_sec = 0;
                char new_colors[100] = {0};
                
                // --- 1. Extracción del Intervalo (usando strstr y sscanf) ---
                char *interval_key = "\"interval_sec\":";
                char *interval_start = strstr(payload, interval_key);
                if (interval_start) {
                    // Mover el puntero al valor numérico después de la clave
                    interval_start += strlen(interval_key);
                    sscanf(interval_start, "%d", &new_interval_sec);
                }

                // --- 2. Extracción de Colores (usando strstr y strchr) ---
                char *colors_key = "\"colors\":\"";
                char *colors_start = strstr(payload, colors_key);
                if (colors_start) {
                    // Mover el puntero al inicio de la cadena de colores (después de la clave y la primera comilla)
                    colors_start += strlen(colors_key);
                    
                    // Buscar el final de la cadena de colores (la comilla doble de cierre)
                    char *colors_end = strchr(colors_start, '\"');
                    
                    if (colors_end) {
                        // Calculamos la longitud de la lista de colores
                        int len = colors_end - colors_start;
                        
                        if (len > 0 && len < sizeof(new_colors)) {
                            // Copiamos solo la lista de colores (ej: "red,green,blue")
                            strncpy(new_colors, colors_start, len);
                            new_colors[len] = '\0'; // Aseguramos el null-terminator
                        }
                    }
                }
                
                // --- 3. Aplicar Configuración Global ---
                if (new_interval_sec >= 1) { // Mínimo 1 segundo
                    current_interval_ms = new_interval_sec * 1000;
                    ESP_LOGI("MQTT", "Nuevo intervalo establecido: %d ms", current_interval_ms);
                }
                
                if (strlen(new_colors) > 0) {
                    strncpy(current_colors, new_colors, sizeof(current_colors) - 1);
                    current_colors[sizeof(current_colors) - 1] = '\0';
                    ESP_LOGI("MQTT", "Nueva secuencia de colores: %s", current_colors);
                }
            }
            break;
        default:
            break;
    }
}

// --- FUNCIONES DE INICIO DE MQTT Y WIFI (No modificadas) ---

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // Se asume WPA2
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// --- FUNCIÓN PRINCIPAL ---

void app_main(void)
{
    // Inicialización de NVS (necesario para Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicialización de Wi-Fi
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // Configuración e inicialización del LED RGB WS2812 (RMT)
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_NUMBERS,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGI(TAG, "LED RGB WS2812 inicializado correctamente");

    // Inicializa MQTT
    mqtt_app_start();

    // Crea la tarea para cambiar el color del LED y publicar en MQTT
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, &led_task_handle);
}