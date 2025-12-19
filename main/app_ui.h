#pragma once


void mp3_player_init(void);
void music_ui(void);

void ai_gui_in(void);
void ai_gui_out(void);

void ai_play(void);
void ai_pause(void);
void ai_resume(void);
void ai_prev_music(void);
void ai_next_music(void);
void ai_volume_up(void);
void ai_volume_down(void);

// 在 SPIFFS 中播放预先生成的提示音频文件
// 例如 /spiffs/prompt_add.wav, /spiffs/prompt_remove.wav, /spiffs/prompt_show.wav
void ui_play_prompt_add(void);
void ui_play_prompt_remove(void);
void ui_play_prompt_show(void);



