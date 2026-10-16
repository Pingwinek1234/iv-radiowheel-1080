// Radio Wheel IV - iv-sdk port for GTA IV / EFLC 1.0.8.0
//
// Original mod: "Radio Wheel IV" by gennariarmando_AG (plugin-sdk, GTA IV Complete Edition).
// This port uses only what iv-sdk exposes for 1.0.8.0: script natives (radio, text, sprites,
// sticks, mouse), CTxdStore (to load radiowheel*.wtd) and the pad/script hooks.
//
// Place this folder in iv-sdk/examples/RadioWheelIV/ so the include below resolves.
#include "../../../include/IVSDK.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace RadioWheelIV
{
	static const float kPi = 3.14159265358979f;
	static const int   kMaxStations = 30;     // size of CAERadioTrackManager::m_aRadioStations
	static const int   kMaxSprites = 23;      // radio_0 .. radio_22 in radiowheel*.wtd
	static const DWORD kHoldToOpenMs = 250;   // hold time before the wheel opens (same as the original)
	static const DWORD kQuickSwitchHudMs = 2000;
	static const DWORD kRetuneStepMs = 120;   // spacing between queued retune commands

	// ------------------------------------------------------------------ settings (RadioWheelIV.ini)
	struct Settings
	{
		bool screenOverlay = true;      // dim + tint the screen while the wheel is open
		bool slowMotion = true;         // slow the game down while the wheel is open
		bool quickSwitchHud = true;     // show our own icon + name after a quick next/prev tap
		bool blockGameRadioInput = true;// stop the game reacting to next/prev radio while we use them
		int  font = 0;                  // SET_TEXT_FONT id used for the station name
	};
	static Settings cfg;

	static bool ParseBool(const std::string& v, bool def)
	{
		std::string s;
		for (char c : v) s += (char)tolower((unsigned char)c);
		if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
		if (s == "false" || s == "0" || s == "no" || s == "off") return false;
		return def;
	}

	// Same "key value" format as the original ini (';' starts a comment).
	static void ReadSettings()
	{
		char path[MAX_PATH] = {};
		GetModuleFileNameA(plugin::GetCurrentModule(), path, MAX_PATH);
		char* dot = strrchr(path, '.');
		if (!dot) return;
		strcpy(dot, ".ini");

		FILE* f = fopen(path, "r");
		if (!f) return;
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			std::string l = line;
			size_t c = l.find(';');
			if (c != std::string::npos) l.erase(c);
			size_t b = l.find_first_not_of(" \t\r\n");
			if (b == std::string::npos) continue;
			l.erase(0, b);
			size_t e = l.find_last_not_of(" \t\r\n");
			l.erase(e + 1);
			size_t sp = l.find_first_of(" \t=");
			if (sp == std::string::npos) continue;
			std::string key = l.substr(0, sp), val = l.substr(sp);
			val.erase(0, val.find_first_not_of(" \t="));
			std::string k;
			for (char ch : key) k += (char)tolower((unsigned char)ch);

			if (k == "bscreenoverlay") cfg.screenOverlay = ParseBool(val, cfg.screenOverlay);
			else if (k == "bslowmotion") cfg.slowMotion = ParseBool(val, cfg.slowMotion);
			else if (k == "bquickswitchhud") cfg.quickSwitchHud = ParseBool(val, cfg.quickSwitchHud);
			else if (k == "bblockgameradioinput") cfg.blockGameRadioInput = ParseBool(val, cfg.blockGameRadioInput);
			else if (k == "nfont") cfg.font = atoi(val.c_str());
		}
		fclose(f);
	}

	// ------------------------------------------------------------------ state
	struct PadSnapshot
	{
		bool prevHeld = false, nextHeld = false;
		int  stickX = 0, stickY = 0;   // right stick
		int  mouseX = 0, mouseY = 0;   // mouse delta this frame
	};
	static PadSnapshot in;

	static bool  g_active = false;     // player is in a vehicle and the wheel may be used
	static bool  g_open = false;
	static bool  g_slowApplied = false;
	static int   g_numStations = 0;
	static int   g_selected = -1;      // station highlighted in the wheel (-1 = radio off)
	static int   g_assumed = -1;       // station we believe the radio is on after queued commands
	static CVector2D g_cursor = {};
	static float g_scrW = 1280.0f, g_scrH = 720.0f;

	static bool  g_prevWasHeld = false, g_nextWasHeld = false;
	static DWORD g_holdStart = 0;
	static DWORD g_quickHudUntil = 0;

	// queued retune commands: +1 = up, -1 = down, >=100 = jump to station (value - 100)
	static std::vector<int> g_queue;
	static DWORD g_lastStep = 0;

	// textures
	static CSprite2d g_sprOff, g_sprCurrent, g_sprStation[kMaxSprites];
	static int  g_loadedEpisode = -1;
	static int  g_txdSlot[3] = { -2, -2, -2 };   // -2 = not tried, -1 = failed

	// ------------------------------------------------------------------ helpers
	static float ScaleY(float v) { return v * g_scrH / 720.0f; }
	static float ScaleX(float v) { return ScaleY(v); } // original ScaleX reduces to this for the wheel's square icons

	static void UpdateScreenSize()
	{
		int w = 0, h = 0;
		Scripting::GET_SCREEN_RESOLUTION(&w, &h);
		if (w > 0 && h > 0) { g_scrW = (float)w; g_scrH = (float)h; }
	}

	struct ScriptContext // lets natives be called outside processScriptsEvent (same trick the SDK uses)
	{
		uint32_t bak;
		ScriptContext()
		{
			bak = CTheScripts::m_pCurrentThread;
			CTheScripts::m_pCurrentThread = (uint32_t)plugin::processScriptsEvent::threadDummy;
		}
		~ScriptContext() { CTheScripts::m_pCurrentThread = bak; }
	};

	static int CountStations()
	{
		int n = 0;
		for (int i = 0; i < kMaxStations; i++)
		{
			if (!CAERadioTrackManager::m_aRadioStations[i]) break;
			n++;
		}
		return n;
	}

	static int CurrentRadio()
	{
		unsigned int idx = Scripting::GET_PLAYER_RADIO_STATION_INDEX();
		return idx < (unsigned int)g_numStations ? (int)idx : -1;
	}

	static int GetEpisode()
	{
		unsigned int ep = Scripting::GET_CURRENT_EPISODE();
		return ep > 2 ? 0 : (int)ep;
	}

	static CRGBA EpisodeColour()
	{
		eHudColour c = HUD_COLOUR_GREEN;
		if (GetEpisode() == 1) c = HUD_COLOUR_MENU_YELLOW_DARK;
		else if (GetEpisode() == 2) c = HUD_COLOUR_MENU_BLUE;
		return CHudColours::ms_HudColour[c];
	}

	// ------------------------------------------------------------------ textures
	static int LoadTxdVariant(int variant)
	{
		if (g_txdSlot[variant] != -2)
			return g_txdSlot[variant];

		static const char* suffix[3] = { "", "_tlad", "_tbogt" };
		std::string slotName = std::string("radiowheel") + suffix[variant];
		std::string path = std::string("platform:/textures/radiowheel") + suffix[variant];

		int slot = CTxdStore::AddTxdSlot((char*)slotName.c_str());
		CTxdStore::AddRef(slot);
		g_txdSlot[variant] = CTxdStore::LoadTxd(slot, (char*)path.c_str()) ? slot : -1;
		return g_txdSlot[variant];
	}

	static void ClearSprites()
	{
		g_sprOff.m_pTexture = nullptr;
		g_sprCurrent.m_pTexture = nullptr;
		for (auto& s : g_sprStation) s.m_pTexture = nullptr;
	}

	static void LoadSprites(int episode)
	{
		ClearSprites();

		int slot = LoadTxdVariant(episode);
		if (slot < 0 && episode != 0)
			slot = LoadTxdVariant(0);   // same fallback as the original mod
		g_loadedEpisode = episode;
		if (slot < 0)
			return;

		CTxdStore::PushCurrentTxd();
		CTxdStore::SetCurrentTxd(slot);

		g_sprOff.SetTexture((char*)"radio_off");
		g_sprCurrent.SetTexture((char*)"radio_current");
		for (int i = 0; i < kMaxSprites; i++)
		{
			char name[32];
			sprintf(name, "radio_%d", i);
			g_sprStation[i].SetTexture(name);
		}

		CTxdStore::PopCurrentTxd();
	}

	static const CSprite2d& SpriteFor(int station)
	{
		if (station < 0) return g_sprOff;
		if (station >= kMaxSprites) return g_sprOff;
		return g_sprStation[station];
	}

	// ------------------------------------------------------------------ drawing
	// cx, cy, hw, hh are in pixels (centre + half size); the native wants 0-1 screen fractions.
	static void DrawSprite(const CSprite2d& s, float cx, float cy, float hw, float hh, int r, int g, int b, int a)
	{
		if (!s.m_pTexture)
			return;
		Scripting::DRAW_SPRITE((unsigned int)(uintptr_t)s.m_pTexture,
			cx / g_scrW, cy / g_scrH, (2.0f * hw) / g_scrW, (2.0f * hh) / g_scrH, 0.0f, r, g, b, a);
	}

	static void IconCoords(int pos, int numIcons, float& x, float& y, float& sw, float& sh)
	{
		const float size = 33.0f;
		float radius = size / sinf(kPi / numIcons);
		float angle = 2.0f * kPi / numIcons;
		int i = pos + 1;

		sw = ScaleX(size);
		sh = ScaleY(size);
		x = (g_scrW / 2.0f) - ScaleX(radius * sinf((i * angle) - kPi));
		y = (g_scrH / 2.0f) - ScaleY(radius * cosf((i * angle) - kPi)) - sh;
	}

	static CVector2D WheelCentre()
	{
		return { g_scrW / 2.0f, g_scrH / 2.0f - ScaleY(32.0f) };
	}

	static void LimitCursor()
	{
		int numIcons = g_numStations + 1;
		float radius = 33.0f / sinf(kPi / numIcons);
		float rx = ScaleX(radius), ry = ScaleY(radius);
		CVector2D c = WheelCentre();
		float dx = g_cursor.x - c.x, dy = g_cursor.y - c.y;
		float f = sqrtf((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry));
		if (f > 1.0f)
		{
			g_cursor.x = c.x + dx / f;
			g_cursor.y = c.y + dy / f;
		}
	}

	static std::string Narrow(const wchar_t* w)
	{
		std::string s;
		if (!w) return s;
		for (; *w; w++)
			s += (*w < 256) ? (char)*w : '?';
		return s;
	}

	static std::string StationLabel(int station)
	{
		if (station < 0)
			return "Radio Off";

		CRadioStation* st = CAERadioTrackManager::m_aRadioStations[station];
		if (!st)
			return "";

		std::string key(st->m_sName, strnlen(st->m_sName, sizeof(st->m_sName)));
		std::string text = Narrow(TheText.Get(key.c_str()));
		// TheText returns a placeholder for unknown keys; fall back to a readable version of the key
		if (text.empty() || text.find("NULL") != std::string::npos)
		{
			text = key;
			std::replace(text.begin(), text.end(), '_', ' ');
		}
		return text;
	}

	static void DrawRadioName(float x, float y, int station)
	{
		std::string label = StationLabel(station);
		if (label.empty())
			return;

		Scripting::SET_TEXT_FONT(cfg.font);
		Scripting::SET_TEXT_PROPORTIONAL(true);
		Scripting::SET_TEXT_BACKGROUND(false);
		Scripting::SET_TEXT_EDGE(true, 0, 0, 0, 255);
		Scripting::SET_TEXT_DROPSHADOW(true, 0, 0, 0, 255);
		Scripting::SET_TEXT_COLOUR(255, 255, 255, 255);
		Scripting::SET_TEXT_CENTRE(true);
		Scripting::SET_TEXT_LINE_HEIGHT_MULT(1.4f);
		Scripting::SET_TEXT_SCALE(0.16f, 0.38f);
		Scripting::SET_TEXT_WRAP(0.0f, 1.0f);
		Scripting::DISPLAY_TEXT_WITH_LITERAL_STRING(x, y, "STRING", label.c_str());
	}

	static void DrawWheel()
	{
		if (cfg.screenOverlay)
		{
			Scripting::DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 0, 0, 0, 110);
			CRGBA c = EpisodeColour();
			Scripting::DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, c.r, c.g, c.b, 25);
		}

		int numIcons = g_numStations + 1;
		for (int pos = -1; pos < g_numStations; pos++)
		{
			float x, y, sw, sh;
			IconCoords(pos, numIcons, x, y, sw, sh);
			DrawSprite(SpriteFor(pos), x, y, sw, sh, 255, 255, 255, pos == g_selected ? 255 : 50);
		}

		float x, y, sw, sh;
		IconCoords(g_selected, numIcons, x, y, sw, sh);
		CRGBA c = EpisodeColour();
		DrawSprite(g_sprCurrent, x, y, sw + ScaleX(4.0f), sh + ScaleY(4.0f), c.r, c.g, c.b, 255);

		DrawRadioName(0.5f, 0.395f, g_selected);
	}

	static void DrawQuickSwitch()
	{
		float x = g_scrW / 2.0f;
		float y = ScaleY(152.5f);
		float s = ScaleY(33.0f);

		DrawSprite(SpriteFor(g_assumed), x, y, s, s, 255, 255, 255, 255);
		CRGBA c = EpisodeColour();
		DrawSprite(g_sprCurrent, x, y, s + ScaleY(4.0f), s + ScaleY(4.0f), c.r, c.g, c.b, 255);
		DrawRadioName(0.5f, 0.288f, g_assumed);
	}

	// ------------------------------------------------------------------ radio control
	// Plans the native calls needed to go from `from` to `to` (-1 = off). The radio cycle used by
	// the game is: station 0 .. N-1, off, station 0 ...
	static void PlanRetune(int from, int to)
	{
		g_queue.clear();
		if (from == to)
			return;

		const int n = g_numStations;
		if (to >= 0)
		{
			if (from < 0)
			{
				// radio is off: one step turns it back on, then jump to the wanted station
				if (to == n - 1)
					g_queue.push_back(-1);
				else
				{
					g_queue.push_back(+1);
					if (to != 0)
						g_queue.push_back(100 + to);
				}
			}
			else
				g_queue.push_back(100 + to);
		}
		else
		{
			// go to "off": walk the cycle in the shorter direction
			int up = n - from;
			int down = from + 1;
			if (up <= down)
				for (int i = 0; i < up; i++) g_queue.push_back(+1);
			else
				for (int i = 0; i < down; i++) g_queue.push_back(-1);
		}
		g_assumed = to;
	}

	static void ProcessRetuneQueue()
	{
		if (g_queue.empty())
			return;
		DWORD now = GetTickCount();
		if (now - g_lastStep < kRetuneStepMs)
			return;
		g_lastStep = now;

		int cmd = g_queue.front();
		g_queue.erase(g_queue.begin());
		if (cmd == +1) Scripting::RETUNE_RADIO_UP();
		else if (cmd == -1) Scripting::RETUNE_RADIO_DOWN();
		else Scripting::RETUNE_RADIO_TO_STATION_INDEX((unsigned int)(cmd - 100));
	}

	static void QuickSwitch(bool next)
	{
		int cur = CurrentRadio();
		if (!g_queue.empty()) cur = g_assumed;

		int target;
		if (next) target = (cur == g_numStations - 1) ? -1 : cur + 1;
		else target = (cur == -1) ? g_numStations - 1 : cur - 1;

		PlanRetune(cur, target);
		g_quickHudUntil = GetTickCount() + kQuickSwitchHudMs;
	}

	// ------------------------------------------------------------------ wheel open / close
	static void OpenWheel()
	{
		g_open = true;
		g_assumed = CurrentRadio();
		g_selected = g_assumed;
		g_queue.clear();

		// start with the cursor on the current station
		float x, y, sw, sh;
		IconCoords(g_selected, g_numStations + 1, x, y, sw, sh);
		g_cursor = { x, y };

		if (cfg.slowMotion && !g_slowApplied)
		{
			CTimer::ms_fTimeScale = 0.1f;
			g_slowApplied = true;
		}
	}

	static void CloseWheel()
	{
		if (g_slowApplied)
		{
			CTimer::ms_fTimeScale = 1.0f;
			g_slowApplied = false;
		}
		g_open = false;
	}

	static void UpdateCursor()
	{
		if (Scripting::IS_USING_CONTROLLER())
		{
			// the stick points straight at an icon (an absolute position reads better than the
			// original's accumulate-and-clamp when using a pad)
			float sx = in.stickX / 128.0f, sy = in.stickY / 128.0f;
			float mag = sqrtf(sx * sx + sy * sy);
			if (mag > 0.4f)
			{
				CVector2D c = WheelCentre();
				g_cursor.x = c.x + (sx / mag) * 10000.0f;
				g_cursor.y = c.y + (sy / mag) * 10000.0f;
			}
		}
		else
		{
			g_cursor.x += in.mouseX * 4.0f;
			g_cursor.y += in.mouseY * 4.0f;
		}
		LimitCursor();
	}

	static void UpdateSelection()
	{
		int numIcons = g_numStations + 1;
		for (int pos = -1; pos < g_numStations; pos++)
		{
			float x, y, sw, sh;
			IconCoords(pos, numIcons, x, y, sw, sh);
			float dx = g_cursor.x - x, dy = g_cursor.y - y;
			if (sqrtf(dx * dx + dy * dy) <= ScaleY(28.0f))
			{
				if (pos != g_selected)
				{
					g_selected = pos;
					PlanRetune(g_assumed, pos);
				}
				break;
			}
		}
	}

	// ------------------------------------------------------------------ per-frame logic
	static bool CanUseWheel()
	{
		if (g_numStations <= 0 || CCutsceneMgr::IsRunning() || CTimer::GetIsPaused())
			return false;

		Scripting::Ped ped;
		Scripting::GET_PLAYER_CHAR(Scripting::GET_PLAYER_ID(), &ped);
		if (!Scripting::IS_PLAYER_PLAYING(Scripting::GET_PLAYER_ID()) || Scripting::IS_CHAR_DEAD(ped))
			return false;

		return Scripting::IS_CHAR_IN_ANY_CAR(ped) || Scripting::IS_CHAR_IN_ANY_BOAT(ped) || Scripting::IS_CHAR_IN_ANY_HELI(ped);
	}

	// runs every frame from CGame::Process (script context)
	static void OnScriptsFrame()
	{
		UpdateScreenSize();

		int episode = GetEpisode();
		if (episode != g_loadedEpisode)
			LoadSprites(episode);

		g_numStations = CountStations();
		g_active = CanUseWheel();
		if (!g_active)
		{
			if (g_open) CloseWheel();
			g_queue.clear();
			g_holdStart = 0;
			g_prevWasHeld = in.prevHeld;
			g_nextWasHeld = in.nextHeld;
			return;
		}

		DWORD now = GetTickCount();
		bool prevReleased = g_prevWasHeld && !in.prevHeld;
		bool nextReleased = g_nextWasHeld && !in.nextHeld;

		if (in.prevHeld)
		{
			if (!g_holdStart)
				g_holdStart = now;
			if (!g_open && now - g_holdStart > kHoldToOpenMs)
				OpenWheel();
			if (g_open)
			{
				UpdateCursor();
				UpdateSelection();
			}
		}
		else
		{
			if (g_open)
				CloseWheel();
			else
			{
				if (nextReleased)
					QuickSwitch(true);
				else if (prevReleased && g_holdStart && now - g_holdStart <= kHoldToOpenMs)
					QuickSwitch(false);
			}
			g_holdStart = 0;
		}

		g_prevWasHeld = in.prevHeld;
		g_nextWasHeld = in.nextHeld;

		ProcessRetuneQueue();

		if (g_open)
			DrawWheel();
		else if (cfg.quickSwitchHud && GetTickCount() < g_quickHudUntil)
			DrawQuickSwitch();
	}

	// runs for every pad each frame; we only care about the player's pad
	static void ZeroInput(CPad* pad, int id)
	{
		pad->m_aValues[id].m_nCurrentValue = 0;
		pad->m_aValues[id].m_nLastValue = 0;
	}

	static void OnPadProcess(CPad* pad)
	{
		if (pad != CPad::GetPad())
			return;

		in.prevHeld = pad->m_aValues[INPUT_VEH_PREV_RADIO].m_nCurrentValue > 0;
		in.nextHeld = pad->m_aValues[INPUT_VEH_NEXT_RADIO].m_nCurrentValue > 0;

		// read the pointing devices before we blank the look inputs below
		if (g_open || in.prevHeld)
		{
			ScriptContext ctx;
			int lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0;
			Scripting::GET_POSITION_OF_ANALOGUE_STICKS(0, &lx, &ly, &rx, &ry);
			Scripting::GET_MOUSE_INPUT(&mx, &my);
			in.stickX = rx; in.stickY = ry;
			in.mouseX = mx; in.mouseY = my;
		}
		else
		{
			in.stickX = in.stickY = in.mouseX = in.mouseY = 0;
		}

		// the game must not also cycle stations when we handle the same buttons
		if (g_active && cfg.blockGameRadioInput)
		{
			ZeroInput(pad, INPUT_VEH_PREV_RADIO);
			ZeroInput(pad, INPUT_VEH_NEXT_RADIO);
		}

		// keep the camera still while the wheel is open (best effort)
		if (g_open)
		{
			ZeroInput(pad, INPUT_LOOK_LEFT);
			ZeroInput(pad, INPUT_LOOK_RIGHT);
			ZeroInput(pad, INPUT_LOOK_UP);
			ZeroInput(pad, INPUT_LOOK_DOWN);
			ZeroInput(pad, INPUT_VEH_LOOK_LEFT);
			ZeroInput(pad, INPUT_VEH_LOOK_RIGHT);
			ZeroInput(pad, INPUT_MOUSE_UD);
			ZeroInput(pad, INPUT_MOUSE_LR);
		}
	}

	// a new game / save load / episode switch: forget everything tied to the old session
	static void OnIngameStartup()
	{
		CloseWheel();
		g_queue.clear();
		g_quickHudUntil = 0;
		g_holdStart = 0;
		g_loadedEpisode = -1;   // sprites are reloaded on the next frame
		ClearSprites();
	}

	static void OnGameLoad()
	{
		ReadSettings();
	}
}

// ran after the sdk initializes, add all your hooks/events/etc here
void plugin::gameStartupEvent()
{
	plugin::gameLoadEvent::Add(RadioWheelIV::OnGameLoad);
	plugin::ingameStartupEvent::Add(RadioWheelIV::OnIngameStartup);
	plugin::processPadEvent::Add(RadioWheelIV::OnPadProcess);
	plugin::processScriptsEvent::Add(RadioWheelIV::OnScriptsFrame);
}
