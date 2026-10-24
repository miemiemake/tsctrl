// pic_probe: ESP32 test tool for the PIC on ZKTeco C3 / ZEM500-style carrier boards.
//
// The PIC is driven over a 3.3 V TTL UART with ZK's MCU protocol
// (EDK500 guide chapter 9, fp-firmware exfun.c):
//   controller -> PIC : 53 53 CMD ~CMD D0..D16 SUM        (22 bytes, 38 if > 17 data bytes)
//   PIC -> controller : A5                                 (ack for every frame)
//                       53 53 CMD ~CMD D0..D6 SUM          (12 bytes, 13 for CMD 215/216)
//   SUM = low byte of the sum of the data bytes only.
//
// Modes (type "help" in the serial monitor, 115200 baud, newline line ending):
//   sniff : original controller still fitted; ESP32 only listens on both wires.
//   scan  : original controller removed; ESP32 tries each candidate link and baud
//           rate with a harmless "get version" command and reports who answers.
//   talk  : ESP32 acts as the controller on the selected link.
//
// See README.md next to this file for the wiring.

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Candidate links. "tx" is the ESP32 pin wired to the header pin the old
// controller transmits on (to the PIC), "rx" the pin wired to the header pin the
// old controller receives on (from the PIC). Put a 1 k resistor in every tx wire.
// ---------------------------------------------------------------------------
struct Link {
	const char *name;
	int tx;	// ESP32 GPIO -> controller TX header pin
	int rx;	// ESP32 GPIO <- controller RX header pin
};

static const Link LINKS[] = {
	{ "A  J6-15 TXD0 / J6-16 RXD0", 19, 18 },
	{ "B  J6-17 TXD1 / J6-18 RXD1", 26, 25 },
	{ "C  J5-18 TXD2 / J5-17 RXD2", 33, 32 },
};
static const int LINK_COUNT = sizeof(LINKS) / sizeof(LINKS[0]);

// Unconnected pins used as dummy TX while sniffing, so we never drive the bus.
static const int DUMMY_TX1 = 21;
static const int DUMMY_TX2 = 22;

static const uint32_t BAUDS[] = { 115200, 57600, 38400, 19200, 9600 };
static const int BAUD_COUNT = sizeof(BAUDS) / sizeof(BAUDS[0]);

// ---------------------------------------------------------------------------
// MCU protocol constants (fp-firmware exfun.h / exfunio.h)
// ---------------------------------------------------------------------------
#define HDR			0x53
#define ACK			0xA5
#define CMD_KEY			1
#define CMD_BUZZER		3
#define CMD_LOCK		4
#define CMD_VERSION		20
#define CMD_SET_RTC		21
#define CMD_RTC			22
#define CMD_HID			50
#define CMD_HID_AUTO		51
#define CMD_BEEP_ENABLE		56
#define CMD_AUXOUT_LEN		106
#define CMD_ALARM		107
#define CMD_WATCHDOG		108
#define CMD_DOOR		110
#define CMD_BATTERY		111
#define CMD_EXT_WG_IN		215
#define CMD_INT_WG_IN		216

enum Mode { MODE_IDLE, MODE_SNIFF, MODE_TALK };

static Mode mode = MODE_IDLE;
static int curLink = 0;
static uint32_t curBaud = 115200;
static bool feedDog = true;
static uint32_t lastDog = 0;
static uint32_t ackCount = 0;

// ---------------------------------------------------------------------------
// Frame parser, one per UART direction (same state machine as _TT232Check_)
// ---------------------------------------------------------------------------
struct Parser {
	const char *tag;
	int i;
	uint8_t cmd;
	uint8_t data[8];
	uint32_t sum;
	uint8_t raw[16];
	int rawLen;
	bool fromController;	// sniff: frames sent by the original controller are 22/38 bytes
};

static Parser pPic  = { "PIC ", 0, 0, {0}, 0, {0}, 0, false };
static Parser pCtl  = { "CTRL", 0, 0, {0}, 0, {0}, 0, true };

static void printHex(const uint8_t *b, int n)
{
	for (int k = 0; k < n; k++)
		Serial.printf("%02X ", b[k]);
}

static void decodePicFrame(uint8_t cmd, const uint8_t *d)
{
	switch (cmd) {
	case CMD_KEY:
		Serial.printf("  key pressed, code %u\n", d[0]);
		break;
	case CMD_VERSION:
		Serial.printf("  PIC firmware version %u\n", d[0]);
		break;
	case CMD_RTC:
		Serial.printf("  PIC clock 20%02X-%02X-%02X %02X:%02X:%02X (weekday %u)\n",
			      d[6], d[4], d[3], d[2], d[1], d[0], d[5]);
		break;
	case CMD_HID: {
		uint32_t bits = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
		Serial.printf("  card on internal reader, 24 bits 0x%06lX -> facility %lu, card %lu"
			      " (byte order guessed, check against a known card)\n",
			      (unsigned long)bits, (unsigned long)(bits >> 16), (unsigned long)(bits & 0xFFFF));
		break;
	}
	case CMD_HID_AUTO:
		Serial.printf("  card, auto format, %u bits\n", d[0]);
		break;
	case CMD_EXT_WG_IN:
	case CMD_INT_WG_IN:
		Serial.printf("  %s Wiegand input, first byte %u (bit count?), data ",
			      cmd == CMD_EXT_WG_IN ? "external" : "internal", d[0]);
		printHex(d + 1, 7);
		Serial.println();
		break;
	case CMD_DOOR: {
		const char *what = "unknown";
		switch (d[0]) {
		case 1: what = "door sensor alarm"; break;
		case 2: what = "exit button"; break;
		case 3: what = "tamper / forced-open alarm"; break;
		case 4: what = "door opened"; break;
		case 5: what = "door closed"; break;
		}
		Serial.printf("  door event %u = %s, extra byte %u (door/reader index?)\n", d[0], what, d[1]);
		break;
	}
	case CMD_BATTERY:
		Serial.printf("  battery state %u (0 external, 1 internal, 2 none)\n", d[0]);
		break;
	default:
		Serial.printf("  unknown command %u, please note it down\n", cmd);
	}
}

// Feed one byte; prints anything recognised.
static void parseByte(Parser &p, uint8_t ch)
{
	// Data length depends on who sent it and on the command.
	int dataLen;
	if (p.fromController)
		dataLen = 17;	// 22-byte frame; 38-byte frames are rare (LCD text) and show as raw
	else
		dataLen = (p.cmd == CMD_EXT_WG_IN || p.cmd == CMD_INT_WG_IN) ? 8 : 7;

	if (p.i == 0 && ch == ACK) {
		ackCount++;
		Serial.printf("[%s] A5 ack\n", p.tag);
		return;
	}
	if (p.rawLen < (int)sizeof(p.raw))
		p.raw[p.rawLen++] = ch;

	switch (p.i) {
	case 0:
	case 1:
		if (ch == HDR) {
			p.i++;
		} else {
			if (p.i == 0)
				Serial.printf("[%s] stray byte %02X\n", p.tag, ch);
			p.i = 0;
			p.rawLen = 0;
		}
		return;
	case 2:
		p.cmd = ch;
		p.sum = 0;
		p.i++;
		return;
	case 3:
		if ((uint8_t)(ch + p.cmd) == 0xFF) {
			p.i++;
		} else {
			Serial.printf("[%s] bad ~cmd after %02X\n", p.tag, p.cmd);
			p.i = 0;
			p.rawLen = 0;
		}
		return;
	default:
		if (p.i - 4 < dataLen) {
			if (p.i - 4 < (int)sizeof(p.data))
				p.data[p.i - 4] = ch;
			p.sum += ch;
			p.i++;
			return;
		}
		// checksum byte
		Serial.printf("[%s] cmd %3u %s: ", p.tag, p.cmd,
			      ((p.sum & 0xFF) == ch) ? "ok " : "BAD SUM");
		printHex(p.data, dataLen < 8 ? dataLen : 8);
		if (p.fromController && dataLen > 8)
			Serial.print("...");
		Serial.println();
		if (!p.fromController && (p.sum & 0xFF) == ch)
			decodePicFrame(p.cmd, p.data);
		p.i = 0;
		p.rawLen = 0;
	}
}

// ---------------------------------------------------------------------------
// UART setup
// ---------------------------------------------------------------------------
static void stopUarts()
{
	Serial1.end();
	Serial2.end();
	for (int k = 0; k < LINK_COUNT; k++) {
		pinMode(LINKS[k].tx, INPUT);	// high impedance: never drive a wire we are not using
		pinMode(LINKS[k].rx, INPUT);
	}
	pPic.i = pCtl.i = 0;
	pPic.rawLen = pCtl.rawLen = 0;
}

static void startSniff()
{
	stopUarts();
	const Link &l = LINKS[curLink];
	// Serial1 listens to what the old controller sends, Serial2 to what the PIC sends.
	Serial1.begin(curBaud, SERIAL_8N1, l.tx, DUMMY_TX1);
	Serial2.begin(curBaud, SERIAL_8N1, l.rx, DUMMY_TX2);
	mode = MODE_SNIFF;
	Serial.printf("SNIFF on link %s at %lu baud. Listening only.\n", l.name, (unsigned long)curBaud);
	Serial.println("Swipe a card, press the exit button, open the door, unlock from the software.");
}

static void startTalk()
{
	stopUarts();
	const Link &l = LINKS[curLink];
	Serial2.begin(curBaud, SERIAL_8N1, l.rx, l.tx);
	mode = MODE_TALK;
	lastDog = 0;
	Serial.printf("TALK on link %s at %lu baud. ESP32 is now the controller.\n", l.name, (unsigned long)curBaud);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
static void sendFrame(uint8_t cmd, const uint8_t *data, int len)
{
	uint8_t buf[38];
	int frameLen = (len > 17) ? 38 : 22;
	if (len > 33)
		len = 33;
	memset(buf, 0, sizeof(buf));
	buf[0] = buf[1] = HDR;
	buf[2] = cmd;
	buf[3] = 0xFF - cmd;
	uint32_t s = 0;
	for (int k = 0; k < len; k++) {
		buf[4 + k] = data[k];
		s += data[k];
	}
	buf[frameLen - 1] = s & 0xFF;
	Serial.printf("[TX  ] cmd %3u: ", cmd);
	printHex(buf, frameLen);
	Serial.println();
	Serial2.write(buf, frameLen);
}

static void pumpTalk(uint32_t ms)
{
	uint32_t t0 = millis();
	while (millis() - t0 < ms) {
		while (Serial2.available())
			parseByte(pPic, Serial2.read());
		delay(1);
	}
}

// Send and wait for the ack like the firmware does (up to 10 tries, 200 ms each).
static bool sendCmd(uint8_t cmd, const uint8_t *data, int len)
{
	if (mode != MODE_TALK) {
		Serial.println("Not in talk mode. Type: talk");
		return false;
	}
	for (int attempt = 0; attempt < 10; attempt++) {
		uint32_t before = ackCount;
		sendFrame(cmd, data, len);
		pumpTalk(200);
		if (ackCount != before)
			return true;
	}
	Serial.println("No A5 ack after 10 tries.");
	return false;
}

// ---------------------------------------------------------------------------
// Scan: try every link and baud with "get version" (cmd 20), which changes nothing.
// ---------------------------------------------------------------------------
static void scan()
{
	Serial.println("SCAN: old controller must be unplugged. Sending 'get version' on each link.");
	int foundLink = -1;
	uint32_t foundBaud = 0;
	for (int li = 0; li < LINK_COUNT && foundLink < 0; li++) {
		for (int bi = 0; bi < BAUD_COUNT; bi++) {
			curLink = li;
			curBaud = BAUDS[bi];
			startTalk();
			uint32_t before = ackCount;
			sendFrame(CMD_VERSION, nullptr, 0);
			pumpTalk(400);
			if (ackCount != before) {
				foundLink = li;
				foundBaud = curBaud;
				break;
			}
		}
	}
	if (foundLink < 0) {
		stopUarts();
		mode = MODE_IDLE;
		Serial.println("SCAN: nothing answered. Check GND, power to the PIC, and try 'sniff' with the old controller fitted.");
		return;
	}
	curLink = foundLink;
	curBaud = foundBaud;
	startTalk();
	Serial.printf("SCAN: PIC found on link %s at %lu baud. Staying in talk mode.\n",
		      LINKS[curLink].name, (unsigned long)curBaud);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
static void help()
{
	Serial.println();
	Serial.println("pic_probe commands:");
	Serial.println("  link A|B|C      choose the candidate link (default A)");
	Serial.println("  baud N          choose baud rate (default 115200)");
	Serial.println("  sniff           listen only, old controller still fitted");
	Serial.println("  scan            old controller removed: find the PIC");
	Serial.println("  talk            act as controller on current link/baud");
	Serial.println("  stop            release all pins");
	Serial.println("talk mode only:");
	Serial.println("  ver             ask PIC version            (cmd 20)");
	Serial.println("  time            read PIC clock             (cmd 22)");
	Serial.println("  door            ask door / alarm state     (cmd 110)");
	Serial.println("  beep            200 ms buzzer              (cmd 3)");
	Serial.println("  unlock S        lock relay on for S s, 1-10 (cmd 4)");
	Serial.println("  alarm on|off    alarm relay                (cmd 107)");
	Serial.println("  dog on|off      feed PIC watchdog every 10 s (cmd 108, default on)");
	Serial.println("  raw CMD [B ..]  send any command, decimal bytes");
	Serial.println();
}

static void handleLine(String line)
{
	line.trim();
	if (line.length() == 0)
		return;
	String cmd = line;
	String arg = "";
	int sp = line.indexOf(' ');
	if (sp > 0) {
		cmd = line.substring(0, sp);
		arg = line.substring(sp + 1);
		arg.trim();
	}
	cmd.toLowerCase();

	if (cmd == "help" || cmd == "?") {
		help();
	} else if (cmd == "link") {
		arg.toUpperCase();
		int li = arg.length() ? arg[0] - 'A' : -1;
		if (li < 0 || li >= LINK_COUNT) {
			Serial.println("link A, B or C");
			return;
		}
		curLink = li;
		Serial.printf("Link %s selected.\n", LINKS[curLink].name);
		if (mode == MODE_SNIFF) startSniff();
		if (mode == MODE_TALK) startTalk();
	} else if (cmd == "baud") {
		curBaud = arg.toInt();
		if (curBaud < 1200) curBaud = 115200;
		Serial.printf("Baud %lu.\n", (unsigned long)curBaud);
		if (mode == MODE_SNIFF) startSniff();
		if (mode == MODE_TALK) startTalk();
	} else if (cmd == "sniff") {
		startSniff();
	} else if (cmd == "scan") {
		scan();
	} else if (cmd == "talk") {
		startTalk();
	} else if (cmd == "stop") {
		stopUarts();
		mode = MODE_IDLE;
		Serial.println("Stopped, all pins released.");
	} else if (cmd == "ver") {
		sendCmd(CMD_VERSION, nullptr, 0);
		pumpTalk(300);
	} else if (cmd == "time") {
		sendCmd(CMD_RTC, nullptr, 0);
		pumpTalk(500);
	} else if (cmd == "door") {
		sendCmd(CMD_DOOR, nullptr, 0);
		pumpTalk(300);
	} else if (cmd == "beep") {
		uint8_t d[1] = { 10 };	// 10 x 20 ms
		sendCmd(CMD_BUZZER, d, 1);
	} else if (cmd == "unlock") {
		int s = arg.toInt();
		if (s < 1 || s > 10) {
			Serial.println("unlock 1..10 (seconds)");
			return;
		}
		uint8_t d[2] = { (uint8_t)(s * 25), 0 };	// 40 ms units, as in ExAuxOut()
		sendCmd(CMD_LOCK, d, 2);
	} else if (cmd == "alarm") {
		uint8_t d[1] = { (uint8_t)(arg == "on" ? 1 : 0) };
		sendCmd(CMD_ALARM, d, 1);
	} else if (cmd == "dog") {
		feedDog = (arg != "off");
		Serial.printf("Watchdog feeding %s.\n", feedDog ? "on" : "off");
	} else if (cmd == "raw") {
		uint8_t d[33];
		int n = 0;
		int c = -1;
		while (arg.length()) {
			int sp2 = arg.indexOf(' ');
			String tok = sp2 < 0 ? arg : arg.substring(0, sp2);
			arg = sp2 < 0 ? "" : arg.substring(sp2 + 1);
			arg.trim();
			if (c < 0) c = tok.toInt();
			else if (n < 33) d[n++] = tok.toInt();
		}
		if (c < 0) {
			Serial.println("raw CMD [bytes...]");
			return;
		}
		sendCmd(c, d, n);
		pumpTalk(300);
	} else {
		Serial.println("Unknown command, type help");
	}
}

void setup()
{
	Serial.begin(115200);
	delay(500);
	stopUarts();
	Serial.println();
	Serial.println("pic_probe ready. All link pins are inputs until you pick a mode.");
	help();
}

void loop()
{
	static String line;
	while (Serial.available()) {
		char c = Serial.read();
		if (c == '\n' || c == '\r') {
			handleLine(line);
			line = "";
		} else {
			line += c;
		}
	}

	if (mode == MODE_SNIFF) {
		while (Serial1.available())
			parseByte(pCtl, Serial1.read());
		while (Serial2.available())
			parseByte(pPic, Serial2.read());
	} else if (mode == MODE_TALK) {
		while (Serial2.available())
			parseByte(pPic, Serial2.read());
		if (feedDog && millis() - lastDog > 10000) {
			lastDog = millis();
			uint8_t d[1] = { 60 };	// 60 s timeout, firmware default MCUDogTime
			sendCmd(CMD_WATCHDOG, d, 1);
		}
	}
}
