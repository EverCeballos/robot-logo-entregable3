#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"

#define R1 12
#define R2 13
#define R3 14
#define R4 15
#define C1 16
#define C2 17
#define C3 18
#define C4 19

#define OLED_SDA   23
#define OLED_SCL   22
#define OLED_ADDR  0x3C

#define MOTOR1_IN1  0
#define MOTOR1_IN2  2
#define MOTOR2_IN1  4
#define MOTOR2_IN2  5
#define PLUMA_LED  21
#define BTN_REINICIO 25

#define LINK_MANDO   Serial1
#define LINK_ROBOT   Serial2
#define MANDO_TX  32
#define MANDO_RX  34
#define ROBOT_TX  33
#define ROBOT_RX  35
#define BAUDIOS_ENLACE  9600
#define BAUDIOS_MONITOR 115200

#define RESP_TIMEOUT   120000UL
#define PERIODO_PUNTO  150
#define MAX_COMANDO    10
#define MS_POR_CM      100
#define MS_POR_GRADO    10

static_assert(R2 == R1 + 1 && R3 == R1 + 2 && R4 == R1 + 3, "Las filas deben ser pines contiguos");
static_assert(C2 == C1 + 1 && C3 == C1 + 2 && C4 == C1 + 3, "Las columnas deben ser pines contiguos");

#define MASCARA_FILAS (0xFUL << R1)
#define MASCARA_COLS  (0xFUL << C1)

static const char KEYMAP[] = "123A456B789C*0#D";

static void initKeypad() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(R1 + i, OUTPUT);
    digitalWrite(R1 + i, LOW);
    pinMode(C1 + i, INPUT_PULLUP);
  }
}

char readKeypad() {
  if ((REG_READ(GPIO_IN_REG) & MASCARA_COLS) == MASCARA_COLS) return 0;

  char tecla = 0;
  for (uint8_t f = 0; f < 4 && !tecla; f++) {
    REG_WRITE(GPIO_OUT_W1TS_REG, MASCARA_FILAS);
    REG_WRITE(GPIO_OUT_W1TC_REG, (1UL << f) << R1);
    delayMicroseconds(5);
    uint32_t cols = ~REG_READ(GPIO_IN_REG) & MASCARA_COLS;
    if (cols) tecla = KEYMAP[(f << 2) | __builtin_ctz(cols >> C1)];
  }
  REG_WRITE(GPIO_OUT_W1TC_REG, MASCARA_FILAS);
  return tecla;
}

static char teclaNueva() {
  static char ultima = 0;
  char k = readKeypad();
  if (k == ultima) return 0;
  ultima = k;
  if (k) delay(20);
  return k;
}

#define FILA(n)     ((n) * 8)
#define BARRA_Y     FILA(7)
#define MAX_PUNTOS  21

Adafruit_SSD1306 pantalla(128, 64, &Wire, -1);

enum { MENU, INTERACTIVO, PROGRAMA };
static uint8_t     estado = MENU;
static String      linea  = "";
static const char* aviso  = "";
static uint8_t     puntos = 0;

static void texto(uint8_t fila, const char* t) {
  pantalla.setCursor(0, FILA(fila));
  pantalla.print(t);
}

static void dibujar() {
  pantalla.clearDisplay();
  switch (estado) {
    case MENU:
      texto(0, ">>> ROBOTLOGO");
      texto(1, "1> Modo Interactivo");
      texto(2, "2> Modo Programa");
      break;
    case INTERACTIVO:
      texto(0, ">>> MODO INTERACTIVO");
      texto(2, "Escriba comando");
      texto(3, "y presione #");
      texto(5, "Guardar/Salir *");
      pantalla.setCursor(0, FILA(6));
      pantalla.print("> ");
      pantalla.print(linea);
      break;
    case PROGRAMA:
      texto(0, ">>> MODO PROGRAMA");
      texto(2, "1>Cuadrado");
      texto(3, "2>Triangulo");
      texto(4, "3) Ultimo guardado");
      texto(5, "Salir*");
      break;
  }
  pantalla.setCursor(0, BARRA_Y);
  if (puntos > 0) { for (uint8_t i = 0; i < puntos; i++) pantalla.print('.'); }
  else            { pantalla.print(aviso); }
  pantalla.display();
}

enum { RES_OK, RES_ER, RES_PARADO, RES_TIMEOUT };

static uint8_t enviarYEsperar(const char* cmd) {
  while (LINK_MANDO.available()) LINK_MANDO.read();
  LINK_MANDO.println(cmd);
  Serial.printf("[MANDO] -> %s\n", cmd);

  String buf = "";
  bool pidioParar = false;
  uint32_t t0 = millis(), tPunto = 0;
  puntos = 1;
  dibujar();

  for (;;) {

    while (LINK_MANDO.available()) {
      char c = LINK_MANDO.read();
      if (c == '\n' || c == '\r') { buf = ""; continue; }
      if (c != '#')               { buf += c; continue; }
      buf.trim();
      uint8_t res = 255;
      if      (buf == "OK") res = pidioParar ? RES_PARADO : RES_OK;
      else if (buf == "ER") res = RES_ER;
      Serial.printf("[MANDO] <- %s#\n", buf.c_str());
      buf = "";
      if (res != 255) { puntos = 0; return res; }
    }

    if (millis() - tPunto >= PERIODO_PUNTO) {
      tPunto = millis();
      puntos = (puntos % MAX_PUNTOS) + 1;
      dibujar();
    }

    if (!pidioParar && teclaNueva() == '*') {
      LINK_MANDO.print('*');
      pidioParar = true;
      Serial.println("[MANDO] -> * (detener)");
    }

    if (millis() - t0 > RESP_TIMEOUT) { puntos = 0; return RES_TIMEOUT; }
    delay(1);
  }
}

static const char* avisoDe(uint8_t res, const char* ok, const char* er) {
  switch (res) {
    case RES_OK:      return ok;
    case RES_ER:      return er;
    case RES_PARADO:  return "Detenido con *";
    default:          return "Sin respuesta";
  }
}

static const char* const CUADRADO[] = {
  "D#", "A15#", "B90#", "A15#", "B90#", "A15#", "B90#", "A15#", "B90#", "D#"
};
static const char* const TRIANGULO[] = {
  "D#", "A15#", "B120#", "A15#", "B120#", "A15#", "B120#", "D#"
};
#define N_CUADRADO   (sizeof(CUADRADO)  / sizeof(CUADRADO[0]))
#define N_TRIANGULO  (sizeof(TRIANGULO) / sizeof(TRIANGULO[0]))

static void ejecutarPrograma(const char* const* lista, uint8_t cantidad) {
  aviso = "";
  uint8_t res = enviarYEsperar("Z#");
  for (uint8_t i = 0; i < cantidad && res == RES_OK; i++) res = enviarYEsperar(lista[i]);
  aviso = avisoDe(res, "Programa terminado", "Error en el programa");
}

static void teclaMenu(char k) {
  if (k == '1') {
    estado = INTERACTIVO;
    linea = "";
    aviso = "";
    enviarYEsperar("I#");
  } else if (k == '2') {
    estado = PROGRAMA;
    aviso = "";
  } else {
    return;
  }
  dibujar();
}

static void teclaInteractivo(char k) {
  if (k == '*') {
    enviarYEsperar("X#");
    estado = MENU;
    aviso = "";
  } else if (k == '#') {
    if (linea.length() == 0) return;
    String cmd = linea + "#";
    uint8_t res = enviarYEsperar(cmd.c_str());
    linea = "";
    aviso = avisoDe(res, "", "Comando invalido");
  } else if (linea.length() < MAX_COMANDO) {
    linea += k;
    aviso = "";
  }
  dibujar();
}

static void teclaPrograma(char k) {
  switch (k) {
    case '1': ejecutarPrograma(CUADRADO,  N_CUADRADO);  break;
    case '2': ejecutarPrograma(TRIANGULO, N_TRIANGULO); break;
    case '3': aviso = avisoDe(enviarYEsperar("R#"), "Programa terminado", "Sin historial"); break;
    case '*': estado = MENU; aviso = ""; break;
    default:  return;
  }
  dibujar();
}

static void initMando() {
  initKeypad();
  Wire.begin(OLED_SDA, OLED_SCL);
  pantalla.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  pantalla.setTextSize(1);
  pantalla.setTextColor(SSD1306_WHITE);
  LINK_MANDO.begin(BAUDIOS_ENLACE, SERIAL_8N1, MANDO_RX, MANDO_TX);
  dibujar();
}

static void loopMando() {
  char k = teclaNueva();
  if (!k) { delay(1); return; }

  switch (estado) {
    case MENU:        teclaMenu(k);        break;
    case INTERACTIVO: teclaInteractivo(k); break;
    case PROGRAMA:    teclaPrograma(k);    break;
  }
}

#define ACC_PARAR       0b0000
#define ACC_ADELANTE    0b1010
#define ACC_ATRAS       0b0101
#define ACC_DERECHA     0b1001
#define ACC_IZQUIERDA   0b0110

#define MASCARA_MOTORES ((1UL << MOTOR1_IN1) | (1UL << MOTOR1_IN2) | \
                         (1UL << MOTOR2_IN1) | (1UL << MOTOR2_IN2))
#define MASCARA_PLUMA   (1UL << PLUMA_LED)

static void motores(uint8_t n) {
  uint32_t on = ((uint32_t)((n >> 3) & 1) << MOTOR1_IN1) |
                ((uint32_t)((n >> 2) & 1) << MOTOR1_IN2) |
                ((uint32_t)((n >> 1) & 1) << MOTOR2_IN1) |
                ((uint32_t)( n       & 1) << MOTOR2_IN2);
  REG_WRITE(GPIO_OUT_W1TC_REG, MASCARA_MOTORES & ~on);
  REG_WRITE(GPIO_OUT_W1TS_REG, on);
}

#define EE_TAM     512
#define EE_MARCA   0xA5
#define EE_DATOS   2
#define HIST_MAX   250

static uint8_t historialLargo() { return EEPROM.read(1); }

static void historialBorrar() {
  EEPROM.write(0, EE_MARCA);
  EEPROM.write(1, 0);
  EEPROM.commit();
}

static void historialAgregar(const String& cmd) {
  uint8_t n = historialLargo();
  if (n + cmd.length() > HIST_MAX) { Serial.printf("[ROBOT] Historial lleno: no se guarda %s\n", cmd.c_str()); return; }
  for (unsigned i = 0; i < cmd.length(); i++) EEPROM.write(EE_DATOS + n + i, cmd[i]);
  EEPROM.write(1, n + cmd.length());
  EEPROM.commit();
}

static String historialTexto() {
  String h = "";
  for (uint8_t i = 0; i < historialLargo(); i++) h += (char)EEPROM.read(EE_DATOS + i);
  return h;
}

static int historialComandos() {
  int n = 0;
  for (uint8_t i = 0; i < historialLargo(); i++) if (EEPROM.read(EE_DATOS + i) == '#') n++;
  return n;
}

enum { QUIETO, ADELANTE, ATRAS, DERECHA, IZQUIERDA, LEVANTAPLUMA, BAJAPLUMA };

int  comando;
int  valor;
bool plumaAbajo = false;
bool grabando   = false;
bool parado     = false;

int distancia_tiempo(int v) { return v * MS_POR_CM; }
int angulo_tiempo(int v)    { return v * MS_POR_GRADO; }

static bool leeNumero(const char* c, uint8_t* i, int* out) {
  int n = 0; uint8_t digitos = 0;
  while (c[*i] >= '0' && c[*i] <= '9' && digitos < 4) {
    n = n * 10 + (c[*i] - '0');
    (*i)++; digitos++;
  }
  *out = n;
  return digitos > 0;
}

void interpreta_comando(char* cadena) {
  uint8_t i = 0;
  bool conValor = true;
  comando = QUIETO;
  valor = 0;

  switch (cadena[i++]) {
    case 'A':
      if (cadena[i] == 'A') { comando = ATRAS; i++; }
      else                    comando = ADELANTE;
      break;
    case 'B': comando = DERECHA;   break;
    case 'C': comando = IZQUIERDA; break;
    case 'D':
      comando = plumaAbajo ? LEVANTAPLUMA : BAJAPLUMA;
      conValor = false;
      break;
    default:
      return;
  }
  if (conValor && !leeNumero(cadena, &i, &valor)) { comando = QUIETO; return; }
  if (cadena[i] != '#' || cadena[i + 1] != 0)       comando = QUIETO;
}

static void pluma(bool abajo) {
  plumaAbajo = abajo;
  REG_WRITE(abajo ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, MASCARA_PLUMA);
  Serial.println(abajo ? "[ROBOT] PLUMA ABAJO (dibujando)" : "[ROBOT] PLUMA ARRIBA");
}

static void mover(uint8_t accion, int ms) {
  parado = false;
  motores(accion);
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)ms) {
    int c = LINK_ROBOT.peek();
    if (c == '*')                            { LINK_ROBOT.read(); parado = true; break; }
    if (c == '\n' || c == '\r' || c == ' ')    LINK_ROBOT.read();
    delay(1);
  }
  motores(ACC_PARAR);
  if (parado) Serial.println("[ROBOT] PARADO CON *");
}

static void ejecutaComando() {
  switch (comando) {
    case ADELANTE:     Serial.printf("[ROBOT] ADELANTE %d cm\n", valor);        mover(ACC_ADELANTE,  distancia_tiempo(valor)); break;
    case ATRAS:        Serial.printf("[ROBOT] ATRAS %d cm\n", valor);           mover(ACC_ATRAS,     distancia_tiempo(valor)); break;
    case DERECHA:      Serial.printf("[ROBOT] DERECHA %d grados\n", valor);     mover(ACC_DERECHA,   angulo_tiempo(valor));    break;
    case IZQUIERDA:    Serial.printf("[ROBOT] IZQUIERDA %d grados\n", valor);   mover(ACC_IZQUIERDA, angulo_tiempo(valor));    break;
    case BAJAPLUMA:    pluma(true);  break;
    case LEVANTAPLUMA: pluma(false); break;
    default:           motores(ACC_PARAR); break;
  }
  comando = QUIETO;
}

static bool ejecutaHistorial() {
  String h = historialTexto();
  if (h.length() == 0) return false;

  Serial.printf("[ROBOT] Programa guardado: %d comandos\n", historialComandos());
  pluma(false);
  char cad[20];
  int ini = 0;
  while (ini < (int)h.length() && !parado) {
    int fin = h.indexOf('#', ini);
    if (fin < 0) break;
    h.substring(ini, fin + 1).toCharArray(cad, sizeof(cad));
    interpreta_comando(cad);
    ejecutaComando();
    ini = fin + 1;
  }
  pluma(false);
  return true;
}

static void responder(const char* r) {
  LINK_ROBOT.print(r);
  LINK_ROBOT.println('#');
  Serial.printf("[ROBOT] -> %s#\n", r);
}

static String leer_comando_teclado() {

  while (LINK_ROBOT.available() && (isspace(LINK_ROBOT.peek()) || LINK_ROBOT.peek() == '*')) LINK_ROBOT.read();
  if (!LINK_ROBOT.available()) return "";

  String s = LINK_ROBOT.readStringUntil('#');
  s.trim();
  Serial.printf("[ROBOT] recibido: %s#\n", s.c_str());
  return s;
}

static bool atiendeControl(const String& s) {
  if (s == "I") { pluma(false); historialBorrar(); grabando = true;
                  Serial.println("[ROBOT] Historial borrado, grabando"); responder("OK"); return true; }
  if (s == "X") { grabando = false;
                  Serial.printf("[ROBOT] Historial guardado: %d comandos\n", historialComandos()); responder("OK"); return true; }
  if (s == "Z") { pluma(false); responder("OK"); return true; }
  if (s == "R") { responder(ejecutaHistorial() ? "OK" : "ER"); return true; }
  return false;
}

static void atiendeMensaje(String s) {
  if (s.length() == 0) return;
  if (atiendeControl(s)) return;

  s += '#';
  char cad_comando[20];
  if (s.length() >= sizeof(cad_comando)) { responder("ER"); return; }
  s.toCharArray(cad_comando, sizeof(cad_comando));
  interpreta_comando(cad_comando);

  if (comando == QUIETO) { Serial.println("[ROBOT] Comando invalido"); responder("ER"); return; }
  if (grabando) historialAgregar(s);
  ejecutaComando();
  responder("OK");
}

static void initRobot() {
  pinMode(MOTOR1_IN1, OUTPUT);  pinMode(MOTOR1_IN2, OUTPUT);
  pinMode(MOTOR2_IN1, OUTPUT);  pinMode(MOTOR2_IN2, OUTPUT);
  pinMode(PLUMA_LED,  OUTPUT);
  pinMode(BTN_REINICIO, INPUT_PULLUP);
  motores(ACC_PARAR);
  REG_WRITE(GPIO_OUT_W1TC_REG, MASCARA_PLUMA);

  EEPROM.begin(EE_TAM);
  if (EEPROM.read(0) != EE_MARCA) historialBorrar();

  LINK_ROBOT.begin(BAUDIOS_ENLACE, SERIAL_8N1, ROBOT_RX, ROBOT_TX);
  LINK_ROBOT.setTimeout(1000);
  Serial.printf("[ROBOT] Comandos guardados en EEPROM: %d\n", historialComandos());
}

static void loopRobot() {
  atiendeMensaje(leer_comando_teclado());

  if ((REG_READ(GPIO_IN_REG) & (1UL << BTN_REINICIO)) == 0) {
    Serial.println("[ROBOT] Reiniciando...");
    ESP.restart();
  }
  delay(1);
}

static void tareaRobot(void*) {
  for (;;) loopRobot();
}

void setup() {
  Serial.begin(BAUDIOS_MONITOR);
  Serial.println("=== ROBOT DIBUJANTE - ENTREGABLE 3 ===");
  initMando();
  initRobot();
  xTaskCreatePinnedToCore(tareaRobot, "robot", 10240, NULL, 1, NULL, 0);
}

void loop() {
  loopMando();
}