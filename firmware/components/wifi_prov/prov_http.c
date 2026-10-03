#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "prov_ap.h"
#include "prov_form.h"
#include "prov_logic.h"
#include "prov_nvs.h"
#include "wifi_prov.h"

static const char *TAG = "prov.http";

#define SID_HEX_LEN 32 /* 16 random bytes */
#define BODY_MAX 512
#define SCAN_MAX 12

static httpd_handle_t s_server;
static char s_sid[SID_HEX_LEN + 1];

/* ------------------------------------------------------------ pages */

static const char CSS[] =
    "<style>body{font-family:system-ui,sans-serif;margin:0;padding:1.5rem 1.25rem;"
    "background:#101014;color:#e8e8ea;line-height:1.5;max-width:30rem}"
    "h1{font-size:1.3rem;margin:0 0 .25rem}h2{font-size:1.05rem;margin:1.5rem 0 .5rem}"
    "p{margin:.5rem 0;color:#a8a8b0}b{color:#e8e8ea}"
    "a{color:#7fc4ff}"
    "label{display:block;margin:.75rem 0 .25rem}"
    "input,select{width:100%;padding:.6rem;font-size:1rem;border-radius:.4rem;"
    "border:1px solid #33333c;background:#1a1a20;color:#e8e8ea;box-sizing:border-box}"
    "button{margin-top:1rem;padding:.7rem 1.2rem;font-size:1rem;border:0;"
    "border-radius:.4rem;background:#2f6fb0;color:#fff}"
    ".err{color:#ff9b9b}.ok{color:#9bdfa0}.warn{color:#ffd48a}</style>";

static esp_err_t send_page(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* The countdown is live and these pages carry credentials. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", to);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------- auth */

/* A session cookie, so the admin authenticates once rather than on
 * every request. It lives only as long as the portal session does. */
static void new_sid(void)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SID_HEX_LEN; i += 2) {
        uint8_t b = (uint8_t)(esp_random() & 0xFF);
        s_sid[i] = hex[b >> 4];
        s_sid[i + 1] = hex[b & 0x0F];
    }
    s_sid[SID_HEX_LEN] = '\0';
}

static bool request_is_authenticated(httpd_req_t *req)
{
    if (!prov_session_is_authenticated() || s_sid[0] == '\0') {
        return false;
    }
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    const char *p = strstr(cookie, "sid=");
    if (p == NULL) {
        return false;
    }
    p += 4;

    char got[SID_HEX_LEN + 1] = {0};
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ';' && n < SID_HEX_LEN) {
        got[n] = p[n];
        n++;
    }
    got[n] = '\0';
    return prov_ct_str_equal(got, s_sid);
}

static int read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        return -1;
    }
    int got = httpd_req_recv(req, buf, req->content_len);
    if (got <= 0) {
        return -1;
    }
    buf[got] = '\0';
    return got;
}

/* ------------------------------------------------------- handlers */

static esp_err_t login_page(httpd_req_t *req, const char *error)
{
    char body[1400];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Portao</title>%s</head><body>"
             "<h1>Portao</h1><p>Rede <b>%s</b> &middot; fecha em <b>%u</b> s.</p>"
             "%s"
             "<form method=\"POST\" action=\"/login\">"
             "<label for=\"u\">Usuario</label>"
             "<input id=\"u\" name=\"u\" autocomplete=\"username\" autofocus>"
             "<label for=\"p\">Senha</label>"
             "<input id=\"p\" name=\"p\" type=\"password\" autocomplete=\"current-password\">"
             "<button type=\"submit\">Entrar</button></form></body></html>",
             CSS, wifi_prov_ssid(),
             (unsigned)wifi_prov_seconds_left((uint32_t)(esp_timer_get_time() / 1000)),
             error != NULL ? error : "");
    return send_page(req, body);
}

static esp_err_t home_page(httpd_req_t *req)
{
    char body[1200];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Portao</title>%s</head><body>"
             "<h1>Portao</h1><p>Fecha em <b>%u</b> s.</p>"
             "<h2>Opcoes</h2>"
             "<p><a href=\"/wifi\">Configurar WiFi</a></p>"
             "<p><a href=\"/console\">Console</a></p>"
             "<p><a href=\"/logout\">Sair</a> &middot; "
             "<a href=\"/finish\">Encerrar agora</a></p>"
             "</body></html>",
             CSS, (unsigned)wifi_prov_seconds_left((uint32_t)(esp_timer_get_time() / 1000)));
    return send_page(req, body);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return request_is_authenticated(req) ? home_page(req) : login_page(req, NULL);
}

static esp_err_t login_post(httpd_req_t *req)
{
    char body[512];
    char user[64] = {0};
    char pass[128] = {0};

    if (read_body(req, body, sizeof(body)) < 0) {
        return login_page(req, "<p class=\"err\">Formulario invalido.</p>");
    }
    bool parsed = prov_form_field(body, "u", user, sizeof(user)) &&
                  prov_form_field(body, "p", pass, sizeof(pass));

    bool ok = parsed && prov_session_login(user, pass);

    /* The posted password must not linger in a stack buffer. */
    memset(body, 0, sizeof(body));
    memset(pass, 0, sizeof(pass));

    if (!ok) {
        if (!prov_session_is_open()) {
            /* Too many failures: the session is gone and so is the AP. */
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "failed login");
        return login_page(req, "<p class=\"err\">Usuario ou senha incorretos.</p>");
    }

    new_sid();
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict", s_sid);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "admin authenticated");
    return redirect(req, "/");
}

static esp_err_t logout_get(httpd_req_t *req)
{
    prov_session_logout();
    s_sid[0] = '\0';
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    return redirect(req, "/");
}

static esp_err_t finish_get(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return redirect(req, "/");
    }
    /* Answer before the radio goes away, or the browser shows an
     * error instead of the confirmation. */
    esp_err_t err =
        send_page(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                       "<title>Portao</title></head><body>"
                       "<h1>Encerrado</h1><p>A rede de configuracao foi desligada.</p>"
                       "</body></html>");
    prov_session_finish();
    return err;
}

/* -------------------------------------------------------- wifi form */

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:
        return "aberta";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "?";
    }
}

static esp_err_t wifi_page(httpd_req_t *req, const char *message)
{
    if (!request_is_authenticated(req)) {
        return redirect(req, "/");
    }

    /* Scanning needs a station interface, so the AP briefly runs in
     * APSTA. Clients see a short stall while the radio hops channels. */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_scan_config_t scan = {.show_hidden = false};
    uint16_t found = 0;
    static wifi_ap_record_t records[SCAN_MAX];

    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
        found = SCAN_MAX;
        if (esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) {
            found = 0;
        }
    }
    ESP_LOGI(TAG, "scan found %u network(s)", (unsigned)found);

    static char body[4096];
    int n =
        snprintf(body, sizeof(body),
                 "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                 "<title>WiFi</title>%s</head><body>"
                 "<h1>Configurar WiFi</h1>%s"
                 "<form method=\"POST\" action=\"/wifi\">"
                 "<label for=\"s\">Rede encontrada</label>"
                 "<select id=\"s\" name=\"ssid\"><option value=\"\">-- escolher --</option>",
                 CSS, message != NULL ? message : "");

    for (uint16_t i = 0; i < found && n < (int)sizeof(body) - 256; i++) {
        /* WEP is listed but not selectable: saying why is more use
         * than hiding the network the admin is looking for. */
        bool wep = (records[i].authmode == WIFI_AUTH_WEP);
        n += snprintf(body + n, sizeof(body) - n,
                      "<option value=\"%s\"%s>%s (%s, %d dBm)%s</option>",
                      (const char *)records[i].ssid, wep ? " disabled" : "",
                      (const char *)records[i].ssid, auth_name(records[i].authmode),
                      records[i].rssi, wep ? " - nao suportada" : "");
    }

    snprintf(body + n, sizeof(body) - n,
             "</select>"
             "<label for=\"m\">Ou digite o nome (rede oculta)</label>"
             "<input id=\"m\" name=\"manual\" placeholder=\"deixe vazio se escolheu acima\">"
             "<label for=\"p\">Senha</label>"
             "<input id=\"p\" name=\"psk\" type=\"password\" "
             "placeholder=\"vazio para rede aberta\" autocomplete=\"off\">"
             "<button type=\"submit\">Salvar</button></form>"
             "<p><a href=\"/\">Voltar</a></p></body></html>");

    esp_wifi_set_mode(WIFI_MODE_AP);
    return send_page(req, body);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    return wifi_page(req, NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return redirect(req, "/");
    }

    char body[BODY_MAX];
    prov_creds_t creds;
    char chosen[PROV_SSID_MAX + 1] = {0};
    char manual[PROV_SSID_MAX + 1] = {0};
    memset(&creds, 0, sizeof(creds));

    if (read_body(req, body, sizeof(body)) < 0) {
        return wifi_page(req, "<p class=\"err\">Formulario invalido ou longo demais.</p>");
    }

    bool got_ssid = prov_form_field(body, "ssid", chosen, sizeof(chosen));
    bool got_manual = prov_form_field(body, "manual", manual, sizeof(manual));
    /* A missing passphrase field and an empty one mean the same thing
     * here: an open network. prov_form_field leaves the buffer empty
     * either way. */
    (void)prov_form_field(body, "psk", creds.psk, sizeof(creds.psk));
    memset(body, 0, sizeof(body)); /* the passphrase was in here */

    /* A typed name wins: it is the only way to reach a hidden network,
     * and someone who typed one meant it. */
    const char *ssid = (got_manual && manual[0] != '\0') ? manual
                       : (got_ssid && chosen[0] != '\0') ? chosen
                                                         : "";
    snprintf(creds.ssid, sizeof(creds.ssid), "%s", ssid);

    prov_result_t r = prov_check_ssid(creds.ssid);
    if (r == PROV_OK) {
        r = prov_check_psk(creds.psk, creds.psk[0] == '\0');
    }
    if (r != PROV_OK) {
        char msg[192];
        snprintf(msg, sizeof(msg), "<p class=\"err\">%s</p>", prov_result_text(r));
        memset(&creds, 0, sizeof(creds));
        return wifi_page(req, msg);
    }

    esp_err_t err = prov_nvs_save(&creds);
    char msg[256];
    if (err == ESP_OK) {
        snprintf(msg, sizeof(msg),
                 "<p class=\"ok\">Salvo para <b>%s</b>.</p>"
                 "<p class=\"warn\">Ainda nao testado: a conexao sera verificada "
                 "no proximo boot.</p>",
                 creds.ssid);
    } else {
        snprintf(msg, sizeof(msg), "<p class=\"err\">Falha ao gravar (0x%x).</p>",
                 (unsigned)err);
    }
    /* Never keep the passphrase in RAM past the request. */
    memset(&creds, 0, sizeof(creds));
    return wifi_page(req, msg);
}

static esp_err_t console_get(httpd_req_t *req)
{
    if (!request_is_authenticated(req)) {
        return redirect(req, "/");
    }
    char body[1200];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Console</title>%s</head><body><h1>Console</h1>"
             "<p>Em construcao.</p>"
             "<p><a href=\"/\">Voltar</a></p></body></html>",
             CSS);
    return send_page(req, body);
}

/* Anything else, including the connectivity-check URLs phones probe. */
static esp_err_t catch_all(httpd_req_t *req)
{
    /* A POST we do not route still has its body waiting on the socket.
     * Leaving it there desynchronises the connection: the next request
     * starts parsing mid-body and fails as a 400. Read it and throw it
     * away. */
    char sink[128];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = (remaining < (int)sizeof(sink)) ? remaining : (int)sizeof(sink);
        int got = httpd_req_recv(req, sink, want);
        if (got <= 0) {
            break;
        }
        remaining -= got;
    }
    memset(sink, 0, sizeof(sink));
    return redirect(req, "http://" PROV_AP_IP "/");
}

/* ------------------------------------------------------------ setup */

esp_err_t prov_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    s_sid[0] = '\0';

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144; /* the scan page builds a 4 KB document */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/logout", .method = HTTP_GET, .handler = logout_get},
        {.uri = "/finish", .method = HTTP_GET, .handler = finish_get},
        {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/console", .method = HTTP_GET, .handler = console_get},
        /* Last: the wildcard would otherwise swallow the routes above.
         * POST gets the same treatment, so an unrouted POST is a
         * redirect rather than a 405 telling a prober which methods
         * the server knows. */
        {.uri = "/*", .method = HTTP_GET, .handler = catch_all},
        {.uri = "/*", .method = HTTP_POST, .handler = catch_all},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    ESP_LOGI(TAG, "portal at http://%s/", PROV_AP_IP);
    return ESP_OK;
}

void prov_http_stop(void)
{
    if (s_server == NULL) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    memset(s_sid, 0, sizeof(s_sid));
    ESP_LOGI(TAG, "web server stopped");
}