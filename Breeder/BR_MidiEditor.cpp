/******************************************************************************
/ BR_MidiEditor.cpp
/
/ Copyright (c) 2014-2015 Dominik Martin Drzic
/ http://forum.cockos.com/member.php?u=27094
/ https://code.google.com/p/sws-extension
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/
#include "stdafx.h"
#include "BR_MidiEditor.h"
#include "BR_EnvelopeUtil.h"
#include "BR_MidiUtil.h"
#include "BR_Misc.h"
#include "BR_MouseUtil.h"
#include "BR_ProjState.h"
#include "BR_Util.h"
#include "../Fingers/RprMidiCCLane.h"
#include "../SnM/SnM.h"
#include "../SnM/SnM_Chunk.h"
#include "../SnM/SnM_Track.h"
#include "../SnM/SnM_Util.h"
#include "../reaper/localize.h"

/******************************************************************************
* Globals                                                                     *
******************************************************************************/
static preview_register_t g_ItemPreview = {{}, 0};
static bool g_itemPreviewPlaying = false;
static bool g_itemPreviewPaused = false;

/******************************************************************************
* MIDI take preview - separate from arrange preview so they don't exclude     *
* each other out                                                              *
******************************************************************************/
static void MidiTakePreview (int, MediaItem_Take*, MediaTrack*, double, double, double, bool);

static void MidiTakePreviewPlayState (bool play, bool pause, bool rec)
{
	if (!play || rec || (g_itemPreviewPaused && play))
		MidiTakePreview(0, NULL, NULL, 0, 0, 0, false);
}

static void MidiTakePreviewTimer ()
{
	bool stopPreview = true;

	if (g_itemPreviewPlaying)
	{
		#ifdef _WIN32
			EnterCriticalSection(&g_ItemPreview.cs);
		#else
			pthread_mutex_lock(&g_ItemPreview.mutex);
		#endif

		// Have we reached the end?
		stopPreview = (g_ItemPreview.curpos >= g_ItemPreview.src->GetLength());

		#ifdef _WIN32
			LeaveCriticalSection(&g_ItemPreview.cs);
		#else
			pthread_mutex_unlock(&g_ItemPreview.mutex);
		#endif
	}

	if (stopPreview)
		MidiTakePreview(0, NULL, NULL, 0, 0, 0, false);
}

static void MidiTakePreview (int mode, MediaItem_Take* take, MediaTrack* track, double volume, double startOffset, double measureSync, bool pauseDuringPrev)
{
	/* mode: 0 -> stop   *
	*        1 -> start  *
	*        2 -> toggle */

	// First stop any ongoing preview
	RegisterCsurfPlayState(false, MidiTakePreviewPlayState);
	if (g_itemPreviewPlaying)
	{
		if (g_ItemPreview.preview_track)
		{
			StopTrackPreview(&g_ItemPreview);
			SendAllNotesOff((MediaTrack*)g_ItemPreview.preview_track);
		}
		else
		{
			StopPreview(&g_ItemPreview);
		}

		g_itemPreviewPlaying = false;
		plugin_register("-timer",(void*)MidiTakePreviewTimer);
		delete g_ItemPreview.src;

		if (g_itemPreviewPaused && mode != 1) // requesting new preview while old one is still playing shouldn't unpause playback
		{
			if (IsPaused())
				OnPauseButton();
			g_itemPreviewPaused = false;
		}

		// Toggled preview off...treat it as stop
		if (mode == 2)
			mode = 0;
	}

	// About IsRecording: REAPER won't preview anything during recording but extension will still think preview is in progress if we let it continue here
	if (mode == 0 || IsRecording())
		return;

	if (take)
	{
		MediaItem* item         = GetMediaItemTake_Item(take);
		MediaItem_Take* oldTake = GetActiveTake(item);
		bool itemMuteState      = *(bool*)GetSetMediaItemInfo(item, "B_MUTE", NULL);
		double effectiveTakeLen = EffectiveMidiTakeLength(take, true, true);

		GetSetMediaItemInfo(item, "B_MUTE", &g_bFalse);     // needs to be set before getting the source
		SetActiveTake(take);                                // active item take and editor take may differ
		PCM_source* src = ((PCM_source*)item)->Duplicate(); // must be item source otherwise item/take volume won't get accounted for

		if (src && effectiveTakeLen > 0 && effectiveTakeLen > startOffset)
		{
			GetSetMediaItemInfo((MediaItem*)src, "D_POSITION", &g_d0);
			GetSetMediaItemInfo((MediaItem*)src, "D_LENGTH", &effectiveTakeLen);

			if (!g_ItemPreview.src)
			{
				#ifdef _WIN32
					InitializeCriticalSection(&g_ItemPreview.cs);
				#else
					pthread_mutex_init(&g_ItemPreview.mutex, NULL);
				#endif
				g_ItemPreview.loop = false;
			}

			g_ItemPreview.src           = src;
			g_ItemPreview.m_out_chan    = (track) ? (-1) : (0);
			g_ItemPreview.curpos        = startOffset;
			g_ItemPreview.volume        = volume;
			g_ItemPreview.preview_track = track;

			// Pause before preview otherwise MidiTakePreviewPlayState will stop it
			g_itemPreviewPaused = pauseDuringPrev;
			if (g_itemPreviewPaused && IsPlaying() && !IsPaused())
				OnPauseButton();

			if (g_ItemPreview.preview_track)
				g_itemPreviewPlaying = !!PlayTrackPreview2Ex(NULL, &g_ItemPreview, (measureSync) ? (1) : (0), measureSync);
			else
				g_itemPreviewPlaying = !!PlayPreviewEx(&g_ItemPreview, (measureSync) ? (1) : (0), measureSync);

			if (g_itemPreviewPlaying)
			{
				plugin_register("timer",(void*)MidiTakePreviewTimer);
				RegisterCsurfPlayState(true, MidiTakePreviewPlayState);
			}
			else
				delete g_ItemPreview.src;
		}

		SetActiveTake(oldTake);
		GetSetMediaItemInfo(item, "B_MUTE", &itemMuteState);
	}
}

/******************************************************************************
* Commands: MIDI editor - Item preview                                        *
******************************************************************************/
void ME_StopMidiTakePreview (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	MidiTakePreview(0, NULL, NULL, 0, 0, 0, false);
}

void ME_PreviewActiveTake (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	void* midiEditor = MIDIEditor_GetActive();
	if (MediaItem_Take* take = SWS_MIDIEditor_GetTake(midiEditor))
	{
		MediaItem* item = GetMediaItemTake_Item(take);

		vector<int> options = GetDigits((int)ct->user);
		int toggle   = options[0];
		int type     = options[1];
		int selNotes = options[2];
		int pause    = options[3];

		MediaTrack* track     = GetMediaItem_Track(item);
		double      volume    = GetMediaItemInfo_Value(item, "D_VOL");
		double      start     = 0;
		double      measure   = (type  == 3) ? 1 : 0;
		bool        pausePlay = (pause == 2) ? true : false;

		if (type == 2)
		{
			double mousePosition = ME_PositionAtMouseCursor(true, true);
			if (mousePosition != -1)
				start = mousePosition - GetMediaItemInfo_Value(item, "D_POSITION");
			else
				return;
		}

		vector<int> muteState;
		if (selNotes == 2)
		{
			if (!AreAllNotesUnselected(take))
			{
				muteState = MuteUnselectedNotes(take);
				if (type != 2)
				{
					BR_MidiEditor editor(midiEditor);
					double time;
					MIDI_GetNote(take, FindFirstSelectedNote(take, &editor), NULL, NULL, &time, NULL, NULL, NULL, NULL);
					start = MIDI_GetProjTimeFromPPQPos(take, time) - GetMediaItemInfo_Value(item, "D_POSITION");
				}
			}
			else if (type != 2)
			{
				BR_MidiEditor editor(midiEditor);
				int id = FindFirstNote(take, &editor);
				if (id != -1)
				{
					double time;
					MIDI_GetNote(take, id, NULL, NULL, &time, NULL, NULL, NULL, NULL);
					start = MIDI_GetProjTimeFromPPQPos(take, time) - GetMediaItemInfo_Value(item, "D_POSITION");
				}
			}
		}
		MidiTakePreview(toggle, take, track, volume, start, measure, pausePlay);

		if (muteState.size() > 0)
			SetMutedNotes(take, muteState);
	}
}

/******************************************************************************
* Commands: MIDI editor - Misc                                                *
******************************************************************************/
void ME_PlaybackAtMouseCursor (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	PlaybackAtMouseCursor(ct);
}

void ME_CCEventAtEditCursor (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
	if (mouseInfo.GetMidiEditor())
	{
		if (MediaItem_Take* take = SWS_MIDIEditor_GetTake(mouseInfo.GetMidiEditor()))
		{
			double startLimit, endLimit;
			double positionPPQ = GetOriginalPpqPos(take, MIDI_GetPPQPosFromProjTime(take, GetCursorPositionEx(NULL)), NULL, &startLimit, &endLimit);
			if (!CheckBounds(positionPPQ, startLimit, endLimit))
				return;

			int lane, value;
			if (mouseInfo.GetCCLane(&lane, &value, NULL) && value >= 0)
			{
				if (lane == CC_TEXT_EVENTS || lane == CC_SYSEX || lane == CC_BANK_SELECT || lane == CC_VELOCITY || lane == CC_VELOCITY_OFF)
					MessageBox((HWND)mouseInfo.GetMidiEditor(), __LOCALIZE("Can't insert in velocity, text, sysex and bank select lanes","sws_mbox"), __LOCALIZE("SWS/BR - Warning","sws_mbox"), MB_OK);
				else
				{
					bool do14bit    = (lane >= CC_14BIT_START) ? true : false;
					int type        = (lane == CC_PROGRAM) ? (STATUS_PROGRAM) : (lane == CC_CHANNEL_PRESSURE ? STATUS_CHANNEL_PRESSURE : (lane == CC_PITCH ? STATUS_PITCH : STATUS_CC));
					int channel     = SWS_MIDIEditor_GetSetting_int(mouseInfo.GetMidiEditor(), "default_note_chan");
					int msg2        = CheckBounds(lane, 0, 127) ? ((value >> 7) | 0) : (value & 0x7F);
					int msg3        = CheckBounds(lane, 0, 127) ? (value & 0x7F)     : ((value >> 7) | 0);

					int targetLane  = (do14bit) ? lane - CC_14BIT_START : lane;
					int targetLane2 = (do14bit) ? targetLane + 32       : lane;

					MIDI_InsertCC(take, true, false, positionPPQ, type,	channel, (CheckBounds(targetLane, 0, 127) ? targetLane : msg2), msg3);
					if (do14bit)
						MIDI_InsertCC(take, true, false, positionPPQ, type, channel, targetLane2, msg2);

					Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ITEMS, -1);
				}
			}
		}
	}
}

void ME_ShowUsedCCLanesDetect14Bit (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	if (MediaItem_Take* take = MIDIEditor_GetTake(MIDIEditor_GetActive()))
	{
		RprTake rprTake(take);
		if (RprMidiCCLane* laneView = new (nothrow) RprMidiCCLane(rprTake))
		{
			int defaultHeight = 67; // same height FNG versions use (to keep behavior identical)
			set<int> usedCC = GetUsedCCLanes(SWS_MIDIEditor_GetActive(), 2);

			for (int i = 0; i < laneView->countShown(); ++i)
			{
				if (usedCC.find(laneView->getIdAt(i)) == usedCC.end())
					laneView->remove(i--);
			}

			// Special case: Bank select and CC0 (from FNG version to keep behavior identical)
			if (usedCC.find(0) != usedCC.end() && usedCC.find(CC_BANK_SELECT) != usedCC.end() && !laneView->isShown(131))
				laneView->append(131, defaultHeight);

			for (set<int>::iterator it = usedCC.begin(); it != usedCC.end(); ++it)
			{
				if (!laneView->isShown(*it))
					laneView->append(*it, defaultHeight);
				else
				{
					for (int i = 0; i < laneView->countShown(); ++i)
					{
						if (laneView->getIdAt(i) == *it && laneView->getHeight(i) == 0)
							laneView->setHeightAt(i, defaultHeight);
					}
				}
			}

			if (laneView->countShown() == 0)
				laneView->append(-1, 0);

			delete laneView;
			Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ITEMS, -1);
		}
	}
}

void ME_CreateCCLaneLastClicked (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	bool updated = false;
	void* editor = SWS_MIDIEditor_GetActive();
	MediaItem_Take* take = editor ? SWS_MIDIEditor_GetTake(editor) : NULL;

	if (take)
	{
		MediaItem* item = GetMediaItemTake_Item(take);
		int takeId      = GetTakeId(take, item);
		if (takeId >= 0)
		{
			SNM_TakeParserPatcher p(item, CountTakes(item));
			WDL_FastString takeChunk;
			int tkPos, tklen;
			if (p.GetTakeChunk(takeId, &takeChunk, &tkPos, &tklen))
			{
				SNM_ChunkParserPatcher ptk(&takeChunk, false);

				// Check current lanes
				bool lanes[SNM_MAX_CC_LANE_ID + 1];
				memset(&lanes, false, SNM_MAX_CC_LANE_ID + 1);

				char lastLaneId[128] = "";
				int tkFirstPos = 0;
				int laneCpt    = 0;
				int pos = ptk.Parse(SNM_GET_CHUNK_CHAR, 1, "SOURCE", "VELLANE", laneCpt, 1, lastLaneId);
				while (pos > 0)
				{
					if (!tkFirstPos) tkFirstPos = pos;
					lanes[atoi(lastLaneId)] = true; // atoi: 0 on failure, lane 0 won't be used anyway..
					pos = ptk.Parse(SNM_GET_CHUNK_CHAR, 1, "SOURCE", "VELLANE", ++laneCpt, 1, lastLaneId);
				}

				// Insert new lane
				if (tkFirstPos > 0)
				{
					tkFirstPos--;
					int i = 1;
					while (i <= SNM_MAX_CC_LANE_ID && lanes[i])
						i++;
					char newLane[SNM_MAX_CHUNK_LINE_LENGTH] = "";
					if (_snprintfStrict(newLane, sizeof(newLane), "VELLANE %d 50 0\n", i) > 0)
						ptk.GetChunk()->Insert(newLane, tkFirstPos);

					updated = p.ReplaceTake(tkPos, tklen, ptk.GetChunk());
				}
			}
		}
	}
	if (updated)
	{
		Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);

		BR_MidiEditor midiEditor(editor);
		if (midiEditor.IsValid())
		{
			if (HWND keyboardWnd = GetPianoView(midiEditor.GetEditor()))
			{
				RECT r; GetClientRect(keyboardWnd, &r);
				int ccStart = abs(r.bottom - r.top) - midiEditor.GetCCLanesFullheight(true) + 1;

				POINT point;
				point.y = ccStart + MIDI_CC_LANE_CLICK_Y_OFFSET;
				point.x = r.left - r.right;
				SimulateMouseClick(hwnd, point, true);

			}
		}
	}
}

void ME_HideCCLanes (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	int laneToProcess;
	void* midiEditor;
	if ((int)ct->user > 0)
	{
		midiEditor    = SWS_MIDIEditor_GetActive();
		laneToProcess = GetLastClickedVelLane(midiEditor);
	}
	else
	{
		BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
		midiEditor = mouseInfo.GetMidiEditor();
		mouseInfo.GetCCLane(&laneToProcess, NULL, NULL);
	}

	if (midiEditor)
	{
		MediaItem_Take* take = SWS_MIDIEditor_GetTake(midiEditor);
		if (take)
		{
			MediaItem* item = GetMediaItemTake_Item(take);
			int takeId = GetTakeId(take, item);
			if (takeId >= 0)
			{
				SNM_TakeParserPatcher p(item, CountTakes(item));
				WDL_FastString takeChunk;
				int tkPos, tklen;
				if (p.GetTakeChunk(takeId, &takeChunk, &tkPos, &tklen))
				{
					SNM_ChunkParserPatcher ptk(&takeChunk, false);
					LineParser lp(false);

					// Remove lanes
					int laneId = 0;
					WDL_FastString lineLane;
					int firstPos = 0;
					int laneCount = 0;
					while (int position = ptk.Parse(SNM_GET_SUBCHUNK_OR_LINE, 1, "SOURCE", "VELLANE", laneId, -1, &lineLane))
					{
						if (!firstPos)
							firstPos = position - 1;
						++laneCount;

						lp.parse(lineLane.Get());
						if ((abs((int)ct->user) == 1 && lp.gettoken_int(1) == laneToProcess) || (abs((int)ct->user) == 2 && lp.gettoken_int(1) != laneToProcess))
						{
							--laneCount;
							ptk.RemoveLine("SOURCE", "VELLANE", 1, laneId);
						}
						else
							++laneId;

						lineLane.DeleteSub(0, lineLane.GetLength());
					}

					// Make sure at least one vellane is left
					if (firstPos && laneCount == 0)
					{
						char newLane[512] = "";
						if (_snprintfSafe(newLane, sizeof(newLane), "VELLANE -1 0 0\n"))
							ptk.GetChunk()->Insert(newLane, firstPos);
					}

					if (p.ReplaceTake(tkPos, tklen, ptk.GetChunk()))
						Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);
				}
			}
		}
	}
}

void ME_ToggleHideCCLanes (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	void* midiEditor = NULL;
	if (g_midiToggleHideCCLanes.Get()->IsHidden())
	{
		if ((int)ct->user > 0)
		{
			midiEditor = SWS_MIDIEditor_GetActive();
		}
		else
		{
			BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
			midiEditor = mouseInfo.GetMidiEditor();
		}

		if (midiEditor && g_midiToggleHideCCLanes.Get()->Restore(midiEditor))
			Undo_OnStateChangeEx2(NULL, __LOCALIZE("Restore hidden CC lanes", "sws_undo"), UNDO_STATE_ITEMS, -1);
	}
	else
	{
		int laneToKeep;
		bool validLane = true;
		if ((int)ct->user > 0)
		{
			midiEditor = SWS_MIDIEditor_GetActive();
			laneToKeep = GetLastClickedVelLane(midiEditor);
		}
		else
		{
			BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
			midiEditor = mouseInfo.GetMidiEditor();
			validLane = mouseInfo.GetCCLane(&laneToKeep, NULL, NULL);
		}

		if (midiEditor && validLane && g_midiToggleHideCCLanes.Get()->Hide(midiEditor, laneToKeep, (abs((int)ct->user) == 1) ? -1 : abs((int)ct->user)))
			Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ITEMS, -1);
	}

	if (midiEditor)
	{
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED")       );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_50_PX") );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_100_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_150_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_200_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_250_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_300_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_350_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_400_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_450_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_LAST_CLICKED_500_PX"));
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE")         );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_50_PX")   );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_100_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_150_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_200_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_250_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_300_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_350_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_400_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_450_PX")  );
		RefreshToolbar2(SECTION_MIDI_EDITOR, NamedCommandLookup("_BR_ME_TOGGLE_HIDE_ALL_NO_MOUSE_LANE_500_PX")  );
	}
}

void ME_CCToEnvPoints (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	BR_MidiEditor midiEditor(SWS_MIDIEditor_GetActive());
	if (!midiEditor.IsValid() || !GetSelectedEnvelope(NULL))
		return;

	MediaItem_Take* take = MIDIEditor_GetTake(MIDIEditor_GetActive());
	BR_Envelope envelope(GetSelectedEnvelope(NULL));
	if ((int)ct->user < 0)
		envelope.DeleteAllPoints();

	int shape = (abs((int)ct->user) == 1) ? SQUARE : LINEAR;
	bool update = false;

	// Process CC events first
	int id = -1;
	set<int> processed14BitIds;
	double ppqPosLast = -1;
	while ((id = MIDI_EnumSelCC(take, id)) != -1)
	{
		int chanMsg, channel, msg2, msg3;
		double ppqPos;
		if (MIDI_GetCC(take, id, NULL, NULL, &ppqPos, &chanMsg, &channel, &msg2, &msg3) && midiEditor.IsCCVisible(take, id))
		{
			double value = -1;
			double max = -1;

			if (chanMsg == STATUS_CC)
			{
				if (ppqPos != ppqPosLast)
					processed14BitIds.clear();

				if (processed14BitIds.find(id) == processed14BitIds.end())
				{
					value = (double)msg3;
					max   = 127;

					// Check for 14bit
					if (CheckBounds(msg2, 0, 32))
					{
						if (midiEditor.FindCCLane(msg2 + CC_14BIT_START) != -1)
						{
							int tmpId = id;
							while ((tmpId = MIDI_EnumSelCC(take, tmpId)) != -1)
							{
								double ppqPos2; int chanMsg2, channel2, nextMsg2, nextMsg3;
								MIDI_GetCC(take, tmpId, NULL, NULL, &ppqPos2, &chanMsg2, &channel2, &nextMsg2, &nextMsg3);
								if (ppqPos2 > ppqPos)
									break;
								if (chanMsg2 == STATUS_CC && msg2 == nextMsg2 - 32 && channel == channel2)
								{
									value = (double)((msg3 << 7) | nextMsg3);
									max   = 16383;
									processed14BitIds.insert(tmpId);
									break;
								}
							}
						}
					}
				}

				ppqPosLast = ppqPos;
			}
			else if (chanMsg == STATUS_PITCH)
			{
				value = (double)((msg3 << 7) | msg2);
				max   = 16383;
			}
			else if (chanMsg == STATUS_PROGRAM || chanMsg == STATUS_CHANNEL_PRESSURE)
			{
				value = (double)(msg2);
				max   = 127;
			}

			if (value != -1)
			{
				MediaItem* item = GetMediaItemTake_Item(take);
				double itemStart = GetMediaItemInfo_Value(item, "D_POSITION");
				double itemEnd   = itemStart + GetMediaItemInfo_Value(item, "D_LENGTH");
				double sourceLenPpq = GetMidiSourceLengthPPQ(take);

				double newValue = TranslateRange(value, 0, max, envelope.LaneMinValue(), envelope.LaneMaxValue());
				while (true)
				{
					double position = MIDI_GetProjTimeFromPPQPos(take, ppqPos);
					if (CheckBounds(position, itemStart, itemEnd))
					{
						if (envelope.CreatePoint(envelope.CountPoints(), MIDI_GetProjTimeFromPPQPos(take, ppqPos), newValue, shape, 0, false, true))
							update = true;
					}
					else if (position > itemEnd)
					{
						break;
					}
					ppqPos += sourceLenPpq;
				}
			}
		}
	}

	// Velocity next
	id = -1;
	while ((id = MIDI_EnumSelNotes(take, id)) != -1)
	{
		int channel, velocity;
		double ppqPos;
		if (MIDI_GetNote(take, id, NULL, NULL, &ppqPos, NULL, &channel, NULL, &velocity) && midiEditor.IsNoteVisible(take, id))
		{
			MediaItem* item = GetMediaItemTake_Item(take);
			double itemStart = GetMediaItemInfo_Value(item, "D_POSITION");
			double itemEnd   = itemStart + GetMediaItemInfo_Value(item, "D_LENGTH");
			double sourceLenPpq = GetMidiSourceLengthPPQ(take);

			double newValue = TranslateRange(velocity, 1, 127, envelope.LaneMinValue(), envelope.LaneMaxValue());
			while (true)
			{
				double position = MIDI_GetProjTimeFromPPQPos(take, ppqPos);
				if (CheckBounds(position, itemStart, itemEnd))
				{
					if (envelope.CreatePoint(envelope.CountPoints(), position, newValue, shape, 0, false, true))
						update = true;
				}
				else if (position > itemEnd)
				{
					break;
				}
				ppqPos += sourceLenPpq;
			}
		}
	}

	if (update && envelope.Commit())
		Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ITEMS | UNDO_STATE_TRACKCFG, -1);
}

void ME_CopySelCCToLane (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	void* editor;
	int lane;
	if ((int)ct->user < 0)
	{
		BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
		editor = (mouseInfo.GetCCLane(&lane, NULL, NULL)) ? mouseInfo.GetMidiEditor() : NULL;
	}
	else
	{
		editor = SWS_MIDIEditor_GetActive();
		lane = GetLastClickedVelLane(editor);
	}

	BR_MidiEditor midiEditor(editor);
	if (!midiEditor.IsValid())
		return;

	WDL_PtrList<BR_MidiCCEvents> events;
	for (int i = 0; i < midiEditor.CountCCLanes(); ++i)
		events.Add(new BR_MidiCCEvents(0, midiEditor, midiEditor.GetCCLane(i)));

	if (lane != CC_TEXT_EVENTS && lane != CC_SYSEX && lane != CC_BANK_SELECT && lane != CC_VELOCITY && lane != CC_VELOCITY_OFF)
	{
		bool update  = false;

		double insertPosition = -1;
		for (int i = 0; i < events.GetSize(); ++i)
		{
			if (BR_MidiCCEvents* savedLane = events.Get(i))
			{
				if (savedLane->CountSavedEvents() > 0)
				{
					if (insertPosition == -1)
						insertPosition = savedLane->GetSourcePpqStart();
					else if (savedLane->GetSourcePpqStart() < insertPosition)
						insertPosition = savedLane->GetSourcePpqStart();
					update = true;
				}
			}
		}

		for (int i = 0; i < events.GetSize(); ++i)
		{
			if (BR_MidiCCEvents* savedLane = events.Get(i))
			{
				if (savedLane->CountSavedEvents() > 0 && savedLane->Restore(midiEditor, lane, false, insertPosition, false))
					update = true;
			}
		}
		if (update)
			Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);
	}
	else
		MessageBox((HWND)midiEditor.GetEditor(), __LOCALIZE("Can't copy to velocity, text, sysex and bank select lanes","sws_mbox"), __LOCALIZE("SWS/BR - Warning","sws_mbox"), MB_OK);
}

void ME_SaveCursorPosSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	SaveCursorPosSlot(ct);
}

void ME_RestoreCursorPosSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	RestoreCursorPosSlot(ct);
}

void ME_SaveNoteSelSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	if (MediaItem_Take* take = MIDIEditor_GetTake(MIDIEditor_GetActive()))
	{
		int slot = (int)ct->user;

		for (int i = 0; i < g_midiNoteSel.Get()->GetSize(); ++i)
		{
			if (slot == g_midiNoteSel.Get()->Get(i)->GetSlot())
				return g_midiNoteSel.Get()->Get(i)->Save(take);
		}
		g_midiNoteSel.Get()->Add(new BR_MidiNoteSel(slot, take));
	}
}

void ME_RestoreNoteSelSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	if (MediaItem_Take* take = MIDIEditor_GetTake(MIDIEditor_GetActive()))
	{
		int slot = (int)ct->user;

		for (int i = 0; i < g_midiNoteSel.Get()->GetSize(); ++i)
		{
			if (slot == g_midiNoteSel.Get()->Get(i)->GetSlot())
			{
				if (g_midiNoteSel.Get()->Get(i)->Restore(take))
					Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);
				break;
			}
		}
	}
}

void ME_SaveCCEventsSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	int lane;
	void* midiEditor;
	if ((int)ct->user < 0)
	{
		BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
		midiEditor = (mouseInfo.GetCCLane(&lane, NULL, NULL)) ? mouseInfo.GetMidiEditor() : NULL;
	}
	else
	{
		midiEditor = SWS_MIDIEditor_GetActive();
		lane = GetLastClickedVelLane(midiEditor);
	}

	BR_MidiEditor editor(midiEditor);
	if (editor.IsValid())
	{
		int slot = abs((int)ct->user) - 1;
		if (lane != CC_TEXT_EVENTS && lane != CC_SYSEX && lane != CC_BANK_SELECT)
		{
			for (int i = 0; i < g_midiCCEvents.Get()->GetSize(); ++i)
			{
				if (slot == g_midiCCEvents.Get()->Get(i)->GetSlot())
				{
					g_midiCCEvents.Get()->Get(i)->Save(editor, lane);
					return;
				}
			}
			g_midiCCEvents.Get()->Add(new BR_MidiCCEvents(slot, editor, lane));
		}
		else
			MessageBox((HWND)editor.GetEditor(), __LOCALIZE("Can't save events from text, sysex and bank select lanes","sws_mbox"), __LOCALIZE("SWS/BR - Warning","sws_mbox"), MB_OK);
	}
}

void ME_RestoreCCEventsSlot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	int lane;
	void* midiEditor;
	if ((int)ct->user < 0)
	{
		BR_MouseInfo mouseInfo(BR_MouseInfo::MODE_MIDI_EDITOR_ALL);
		midiEditor = (mouseInfo.GetCCLane(&lane, NULL, NULL)) ? mouseInfo.GetMidiEditor() : NULL;
	}
	else
	{
		midiEditor = SWS_MIDIEditor_GetActive();
		lane = GetLastClickedVelLane(midiEditor);
	}

	BR_MidiEditor editor(midiEditor);
	double editCursorPpq = MIDI_GetPPQPosFromProjTime(editor.GetActiveTake(), GetCursorPosition());
	if (editor.IsValid())
	{
		int slot = abs((int)ct->user) - 1;
		for (int i = 0; i < g_midiCCEvents.Get()->GetSize(); ++i)
		{
			if (slot == g_midiCCEvents.Get()->Get(i)->GetSlot())
			{
				if (lane != CC_TEXT_EVENTS && lane != CC_SYSEX && lane != CC_BANK_SELECT && lane != CC_VELOCITY && lane != CC_VELOCITY_OFF)
				{
					if (g_midiCCEvents.Get()->Get(i)->Restore(editor, lane, false, editCursorPpq))
						Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);
				}
				else
					MessageBox((HWND)editor.GetEditor(), __LOCALIZE("Can't restore to velocity, text, sysex and bank select lanes","sws_mbox"), __LOCALIZE("SWS/BR - Warning","sws_mbox"), MB_OK);
				break;
			}
		}
	}
}

void ME_RestoreCCEvents2Slot (COMMAND_T* ct, int val, int valhw, int relmode, HWND hwnd)
{
	BR_MidiEditor midiEditor;
	double editCursorPpq = MIDI_GetPPQPosFromProjTime(midiEditor.GetActiveTake(), GetCursorPosition());
	if (midiEditor.IsValid())
	{
		int slot = (int)ct->user;
		for (int i = 0; i < g_midiCCEvents.Get()->GetSize(); ++i)
		{
			if (slot == g_midiCCEvents.Get()->Get(i)->GetSlot())
			{
				if (g_midiCCEvents.Get()->Get(i)->Restore(midiEditor, 0, true, editCursorPpq))
					Undo_OnStateChangeEx2(NULL, SWS_CMD_SHORTNAME(ct), UNDO_STATE_ALL, -1);
				break;
			}
		}
	}
}

/******************************************************************************
* Toggle states: MIDI editor - Misc                                           *
******************************************************************************/
int ME_IsToggleHideCCLanesOn (COMMAND_T* ct)
{
	return (int)g_midiToggleHideCCLanes.Get()->IsHidden();
}