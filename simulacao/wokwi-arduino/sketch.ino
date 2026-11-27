// Robô 10x10 — derrubar a garrafa (ESP32 DevKit 38 pinos)
// Mesmas portas do esquemático Robo10x10 (KiCad) e da placa soldada.
//
// Comportamento:
//  - Procurando: gira devagar para a direita (10%) até o ultrassônico ver algo a até 40 cm.
//  - Alvo encontrado: vai para frente em direção a ele (50%).
//  - Infravermelho (na frente do robô) vê qualquer cor que não seja preta: freia na hora,
//    dá ré e volta a procurar.

// HC-SR04 (ultrassônico)
const int TRIG = 22;   // direto no ESP32
const int ECHO = 35;   // passa pelo level shifter: HV1 -> LV1

// TCRT5000 (infravermelho), saída D0
const int IR_D0 = 34;  // passa pelo level shifter: HV4 -> LV4. Preto = HIGH, outra cor = LOW

// TB6612FNG — motor A = direito, motor B = esquerdo. STBY fica ligado direto no 3V3.
const int PWMA = 13, AIN1 = 14, AIN2 = 12;  // amarelo, branco, verde
const int PWMB = 16, BIN1 = 26, BIN2 = 33;  // cinza, roxo, azul

const int BUSCA = 26;            // 10% de 255
const int ATAQUE = 128;          // 50% de 255
const float DIST_ALVO_CM = 40;   // distância de "encontro"
const unsigned long ESPERA_INICIO_MS = 3000;  // tempo para soltar o robô na arena
const int RE = 128;                  // força da ré ao ver a borda (50%)
const unsigned long FREIO_MS = 50;   // freio antes da ré, para matar o embalo para frente
const unsigned long RE_MS = 220;     // duração da ré: ajuste no robô real

// Se no robô real um motor girar para trás quando deveria ir para frente, troque o dele para true.
const bool INVERTE_DIREITO = false;
const bool INVERTE_ESQUERDO = false;

// vel > 0 = frente, vel < 0 = ré, 0 = solto
void motor(int pwm, int in1, int in2, int vel, bool inverte) {
  if (inverte) vel = -vel;
  digitalWrite(in1, vel > 0);
  digitalWrite(in2, vel < 0);
  analogWrite(pwm, abs(vel));
}

void mover(int esq, int dir) {
  motor(PWMA, AIN1, AIN2, dir, INVERTE_DIREITO);
  motor(PWMB, BIN1, BIN2, esq, INVERTE_ESQUERDO);
}

// IN1 = IN2 = HIGH no TB6612 = freio (curto no motor): para mais rápido do que só cortar o PWM.
void freiar() {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, HIGH);
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, HIGH);
}

void setup() {
  Serial.begin(115200);
  // G12 precisa ficar em LOW: é pino de boot do ESP32.
  for (int p : {PWMA, AIN1, AIN2, PWMB, BIN1, BIN2, TRIG}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  pinMode(ECHO, INPUT);
  pinMode(IR_D0, INPUT);  // G34 não tem pull-up interno; o level shifter já faz esse papel

  Serial.println("Robo 10x10 - comecando em 3 s...");
  delay(ESPERA_INICIO_MS);
}

bool alvo = false;
int estado = -1;      // último movimento aplicado: 0 = busca, 1 = ataque, -1 = reaplicar
float distancia = -1;
bool echoAnterior = false;
unsigned long inicioEcho = 0, ultimoDisparo = 0, ultimaMedida = 0;

void loop() {
  // 1) Borda da arena: checada a cada volta do loop (alguns microssegundos), antes de tudo.
  if (digitalRead(IR_D0) == LOW) {
    freiar();
    Serial.println("COR DIFERENTE DE PRETO! Freando e dando re");
    delay(FREIO_MS);
    mover(-RE, -RE);
    delay(RE_MS);
    // Medida antiga do ultrassônico não vale mais: só ataca de novo com uma leitura nova.
    alvo = false;
    estado = -1;
    return;  // se ainda não estiver no preto, a próxima volta repete freio + ré
  }

  // 2) Ultrassônico sem travar o loop: mede o pulso de ECHO pelas bordas de subida e descida.
  bool echo = digitalRead(ECHO);
  if (echo && !echoAnterior) inicioEcho = micros();
  if (!echo && echoAnterior) {
    distancia = (micros() - inicioEcho) / 58.0;
    alvo = distancia <= DIST_ALVO_CM;
    ultimaMedida = millis();
  }
  echoAnterior = echo;

  // Novo disparo a cada 60 ms (mínimo do HC-SR04), só com o ECHO em repouso.
  if (!echo && millis() - ultimoDisparo >= 60) {
    digitalWrite(TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    ultimoDisparo = millis();
  }
  if (millis() - ultimaMedida > 200) alvo = false;  // sensor sem resposta = sem alvo

  // 3) Movimento: só mexe nos motores quando o estado muda.
  if (alvo != estado) {
    estado = alvo;
    if (alvo) {
      mover(ATAQUE, ATAQUE);
      Serial.printf("ALVO a %.1f cm: indo para frente (50%%)\n", distancia);
    } else {
      mover(BUSCA, -BUSCA);
      Serial.println("Procurando: girando para a direita (10%)");
    }
  }
}
