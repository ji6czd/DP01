#pragma once

#include <cstddef>
#include <cstdint>

namespace config {

// AES-CBCのブロック境界(16バイト)の倍数を維持すること。
inline constexpr size_t kSegmentReadChunkSize = 4096;

// ES8311 DACボリュームレジスタ(0x32)への直値。0x00=ミュート、0xBFで±0dB、
// 0.5dB/stepで0xFFまで(+32dB相当)。とりあえず控えめな値にしてあるので、
// 実機で聴きながら調整すること。
inline constexpr uint8_t kSpeakerVolume = 0xb0;

// VolumeUp/VolumeDownキー1回あたりのボリューム増減幅(同レジスタのスケール)。
inline constexpr uint8_t kVolumeStep = 0x08;

// VolumeUp/VolumeDownキーで動ける範囲(同レジスタのスケール)。
// 上限は0dB(0xBF)。それより上はデジタルで増幅するので、ピークの大きい録音は歪む。
// 下限は完全なミュートにしない(画面が無いので、無音だと故障と区別がつかない)。
// 0x58で約-51.5dB(kSpeakerVolumeから11段下)。
inline constexpr uint8_t kVolumeMin = 0x58;
inline constexpr uint8_t kVolumeMax = 0xbf;
static_assert(kVolumeMin <= kSpeakerVolume && kSpeakerVolume <= kVolumeMax,
              "default volume must be within the key range");

// ボリューム変更後、この時間キー操作が無ければPreferences(NVS)へ書き込む。
// 狙いはフラッシュ寿命ではなく音切れ防止。NVSは追記型なので寿命は事実上問題にならないが
// (32byteエントリ126個でページが埋まり、そこで初めて4KBセクタ消去)、書き込み中は
// 命令キャッシュが無効化され両コアが止まる。セクタ消去は典型40ms前後で、
// kPcmRingBufferBytes分のクッション(約85ms)を大きく削る。連打を最後の1回にまとめて
// この停止の発生頻度を下げるためのデバウンス。
inline constexpr uint32_t kVolumeSaveDebounceMs = 3000;

// チャンネル切り替え後、この時間切り替えが無ければPreferences(NVS)へ書き込む
// (kVolumeSaveDebounceMsと同じ理由。選局を連打しても書き込みは最後の1回だけになる)。
inline constexpr uint32_t kChannelSaveDebounceMs = 3000;

// テンキーによる直接選局は4桁固定: プラットフォーム番号1桁(1=NHK, 2=radiko,
// 3=JCBA, 4=ListenRadio) + そのプラットフォーム内の局番号3桁(001始まり)。例:
// 2013 = radikoの 13局目。コード⇔局の対応は channel_list.h /
// station_id_list.h(生成テーブル)を参照。
// 最後に数字キーを押してからこの時間だけ次の桁を待ち、超えたら入力中の桁を破棄する。
inline constexpr uint8_t kDirectTuneDigits = 4;
inline constexpr uint32_t kDirectTuneTimeoutMs = 2000;

// Command::kDateJump のあと、テンキー入力を絶対日時 ddhhmm(6桁)
// のタイムフリージャンプ として解釈する。桁間タイムアウトは
// kDirectTuneTimeoutMs を流用する。
inline constexpr uint8_t kDateJumpDigits = 6;
static_assert(kDateJumpDigits >= kDirectTuneDigits, "shared digit buffer is sized to kDateJumpDigits");

// タイムフリーで遡れる限界(nowからの秒数)。radikoは通常約8日だが、契約により最大30日まで
// 遡れる。契約有無はAPI応答から判別できないため広い方(30日)に合わせる。7日契約ユーザの
// 扱いは後日検討。相対シークの遡り限界と日時ジャンプの範囲チェックの両方で使う。
inline constexpr int32_t kTimefreeMaxAgeSec = 30 * 24 * 3600 - 3600;  // 約30日(1時間の余裕)

// radikoのmedialist(m3u8)の1セグメント長。ライブ/タイムフリーとも実測で常に5秒
// (TARGETDURATION=5)。セグメント本数 ⇔ プレイリスト生成URLの l=<秒>
// パラメータの 換算に使う(l = 本数 × この値)。
inline constexpr int kRadikoSegmentDurationSec = 5;

// ライブ選局時、初回medialistで要求するセグメント本数(URLの l= に
// ×kRadikoSegmentDurationSec
// して渡る)。少ないほど取得は軽いが初回の前方バッファは薄い。
// 公式ウェブアプリ(radiko-js-player)は呼び出し元が付けた l を捨てて常に
// _length=15 で
// 上書きする(ライブ・タイムフリー共通、2026-09-23確認)。アクセスの形を公式から外さない
// ため、ライブ・タイムフリーとも l=15
// に揃える。以前は切り替え直後の音切れ対策で l=30(6本)にしていた。
inline constexpr int kLiveInitialFetchSegments = 3;  // l=15相当(公式と同じ)

// ライブ再生の開始位置を、取得したmedialistの末尾(ライブエッジ)から何本手前にするか。
// 大きいほど遅延は増えるが、前方の再生バッファ(アンダーラン耐性)と、ウィンドウ先頭
// (期限切れ)までの余裕がともに増える。回線が不安定ならkLiveInitialFetchSegmentsと
// 併せて増やす。2なら「1本を再生中・1本を待機」でエッジから約(2-1)×5秒遅れ。
// medialistが本数未満しか返らなければ先頭(0)から。
// radikoは l=15
// で3本しか返らないので、3=ウィンドウの先頭から(取れる前方バッファの最大)。
// 2(旧 l=15 時代の値)では切り替え直後に音切れしたため、それより1本厚い。
// NHK/ListenRadio(約10秒セグメント)もこの値を使う(エッジから約30秒遅れ)。
inline constexpr int kLiveStartMarginSegments = 3;
static_assert(kLiveStartMarginSegments >= 1, "need at least one segment to start playback");
static_assert(kLiveStartMarginSegments <= kLiveInitialFetchSegments,
              "margin larger than the fetched window just starts at segment 0");

// タイムフリー選局時、初回medialistで要求するセグメント本数(同上×kRadikoSegmentDurationSec)。
// タイムフリーはseek指定時刻ちょうどから先頭を積む(末尾マージンは使わない)。
// medialistのスライドはライブと同じ1×実時間なので、ライブと同じ薄い窓で足りる
// (refreshで前進する)。以前は60(l=300、約5分)で先読みを厚くしていたが、初回medialist
// 本文(約15KB)+セグメント60本ぶんのString/vector/dequeが、TLSセッション共存時の
// ヒープを圧迫していた。厚くしても得られるのはネットワーク停滞への耐性だけ。
// 公式ウェブアプリはタイムフリーも
// l=15(kLiveInitialFetchSegmentsのコメント参照)。
inline constexpr int kTimefreeInitialFetchSegments = 3;  // l=15相当(公式と同じ)

// タイムフリーのプレイリスト作成URLの end_at/to を
// start_at(=seek)からどれだけ先に置くか。
// 公式ウェブアプリは番組表の番組の開始・終了を渡す。m5rajiruは番組表に依存しないので
// 固定幅にするが、実在する番組の長さから外れた範囲(to−ft
// が6時間など)は避けて3時間に する(radiko側の上限は実測で 6h OK / 25h
// で400)。to に達すると medialist に #EXT-X-ENDLIST
// が付くので、radio_stream.cpp がその続きの時刻で張り直す(番組境界・
// セッション境界ともギャップレスに繋がることは実測済み)。
inline constexpr long kTimefreeSessionSpanSec = 3L * 3600L;

// メディアプレイリストの再取得間隔の下限を決める #EXT-X-TARGETDURATION
// が取れなかった ときの既定値(秒)。再取得は RFC 8216 §6.3.4
// どおり、前回の取得で新しいセグメントが あれば
// TARGETDURATION、無ければその半分だけ空ける(公式が使う hls.js と同じ動き)。
inline constexpr int kDefaultTargetDurationSec = 5;

inline constexpr int kMaxConsecutiveSegmentFailures = 5;
inline constexpr int kMaxConsecutivePlaylistFailures = 3;

// 選局(radio_stream.cppのswitchToChannel)の各段階(radiko認証 / URL生成 /
// master取得 / media取得)で、失敗時にリトライする回数と間隔。使い切ったら
// kBootChannelIndex へ
// フォールバックする。以前は無限リトライだったため、特定の局が恒久的に壊れている
// (認証キー変更・新たな403等)と起動がそこで永久にハングし、選局操作も届かなかった。
// 一時的な障害で好みの局を諦めないよう、ある程度は粘る(5回×2秒=1段階あたり10秒)。
inline constexpr int kChannelSwitchRetryCount = 5;
inline constexpr uint32_t kChannelSwitchRetryDelayMs = 2000;

// Wi-Fi再接続のバックオフ(ms)。最後の値で頭打ち。
inline constexpr uint32_t kWifiRetryBackoffMs[] = {500, 1000, 2000, 4000};

// 起動時のNTP時刻同期。configTzTime()に渡すタイムゾーン(POSIX TZ書式。"JST-9"は
// UTC+9・夏時間なし)と、問い合わせ先のNTPサーバ(先頭から順に試される)。
// 1台目は日本標準時のNICT。
inline constexpr char kNtpTimezone[] = "JST-9";
inline constexpr char kNtpServer1[] = "ntp.nict.jp";
inline constexpr char kNtpServer2[] = "pool.ntp.org";
inline constexpr char kNtpServer3[] = "time.google.com";

// 起動時、時刻が入る(1970年より十分未来になる)のをこの時間まで待つ。
// 超えたら同期未完了のまま先へ進む(再生自体は時刻に依存しないため)。
inline constexpr uint32_t kNtpSyncTimeoutMs = 10000;
inline constexpr uint32_t kNtpSyncPollIntervalMs = 200;

// 音声デコーダ(AAC/Opus)の状態を置く静的アリーナ(codec_arena.h)。同時に使うのは
// どちらか1つなので max() で足りる。ヒープに置くとチャンネル切り替えのたびに
// ~30KB の free/malloc
// が走って断片化の種になるため静的にした。実測サイズ(起動ログ "decoder state N
// bytes" で確認できる):
//   AAC(Helix, SBR無効) 20,656B / AAC(SBR有効) +50,788B / Opus stereo 26,520B
// 各デコーダの begin() が実サイズをここと比較し、足りなければ LOGE
// を出して失敗する (黙って壊れない)。-DHELIX_DISABLE_AAC_SBR
// を外すときはこの値を 72KB 以上にすること。
inline constexpr size_t kCodecArenaBytes = 26 * 1024;  // 26,624B(Opus stereo + 104B)

// ネットワーク取得(別コアのタスク)からデコード側へ渡すリングバッファ。中身は
// [codec 1B][len 2B LE][payload] のレコード列(radio_stream.cpp の
// AudioRecord)。 AAC は復号済み ADTS のチャンク(≤kSegmentReadChunkSize)、Opus
// は1パケットが1レコード。 AAC-LC ~96kbps / Opus ~64kbps
// で数秒分のクッションを持たせ、Wi-Fiの瞬断やプレイリスト
// 再取得の待ち時間を吸収する。JCBA では
// burst(kJcbaBurstSec)ぶんがここに先に積まれる。
inline constexpr size_t kAudioStreamBufferBytes = 64 * 1024;
// レコードのペイロード上限(消費側の受信バッファがこのサイズになる)。Opus の
// 20ms パケットは 仕様上最大 1275B(JCBA 実測 115〜263B)。AAC
// のチャンク(kSegmentReadChunkSize)はこれより 大きいが ADTS
// はただのバイト列なので、feedDownstream() が複数レコードに分割して流す。
inline constexpr size_t kAudioRecordMaxPayload = 1536;

// JCBA(WebSocket)接続時にサーバへ頼む先送り秒数。接続直後にこの秒数ぶんの Ogg
// ページが まとめて届き、そのまま前方バッファになる(5秒 ≈
// 40KB、kAudioStreamBufferBytes に収まる)。
inline constexpr int kJcbaBurstSec = 5;
// JCBA でこの時間フレームが1つも届かなければ接続が死んだとみなして繋ぎ直す
// (本家プレーヤーは burst+10秒)。
inline constexpr uint32_t kJcbaStallTimeoutMs = 15000;
// jcbaReadPage()
// でフレーム開始を待つ時間。これが選局要求への応答遅れの上限になる。
inline constexpr uint32_t kJcbaReadWaitMs = 100;

// ネットワーク取得タスク。実測の使用ピーク 3,408B(HLS + JCBA の TLS
// ハンドシェイク2ホスト分、 WebSocket 受信を含む。定期ログの
// stack(fetch/decode) で確認できる)。
inline constexpr uint32_t kFetchTaskStackBytes = 6144;
inline constexpr int kFetchTaskPriority = 1;
inline constexpr int kFetchTaskCore = 0;  // Arduinoのloop()は既定でcore1で動くため、別コアに配置する。

// AACデコード後、I2S書き込み前のPCM(int16,
// ステレオインターリーブ)のクッション。
// 4×2048サンプル相当(AAC_MAX_NSAMPS=1024/ch(SBR無効時) × 2ch × 2byte =
// 4096byte/frame)、
// 約85ms分。i2sSpeakerWrite()の実時間ペースのブロッキングから、デコード側を切り離すために使う。
inline constexpr size_t kPcmRingBufferBytes = 16 * 1024;
inline constexpr size_t kPcmWriterChunkBytes = 1024;

// チャンネル切り替え時のPCM破棄モードに、この時間以上留まったら強制的に脱出する。
// 正常時はリング1杯分(kPcmRingBufferBytes、24kHzステレオで約170ms)を捨てれば
// 抜けるので、これは「脱出条件が壊れていた場合に無音が続くのを防ぐ」ための保険。
inline constexpr uint32_t kPcmDiscardTimeoutMs = 1000;

// i2sSpeakerIsPlaying()が「再生中」と判定する猶予。最後に実PCM(データ切れの
// 無音埋めではない)をI2Sへ書き込んでからこの時間を超えたら「停止中」とみなす。
inline constexpr uint32_t kSpeakerPlayingTimeoutMs = 500;

// s_audioStreamBufferの消費(デコード)専用タスク。fetchTaskFnと同じcore
// (ネットワーク取得+デコードの「生産側」をまとめる)。
// Opus デコーダは作業領域を C99 VLA でスタックに取る(実測 ≈10KB、PLC
// 含む)ので、 AAC 時代の 6144 から広げた。実機での使用ピークは 11,616B(CELT
// フルバンド・ステレオ、 定期ログの stack(fetch/decode)
// で確認できる)。PLC(+≈0.6KB)や SILK/Hybrid モードは 未計測なので、余裕 ≈2.7KB
// を残してここまで。これ以上詰めるならそれらを踏ませてから。
inline constexpr uint32_t kDecodeTaskStackBytes = 14336;
inline constexpr int kDecodeTaskPriority = 1;
inline constexpr int kDecodeTaskCore = 0;

// PCMリングバッファを実際にI2Sへ書き込む専用タスク。loop()(キー処理)と同じcoreに置き、
// 優先度をloop()より上げることで、PCMが溜まり次第確実にプリエンプトさせる。
inline constexpr uint32_t kI2sWriterTaskStackBytes = 4096;
inline constexpr int kI2sWriterTaskPriority = 2;
inline constexpr int kI2sWriterTaskCore = 1;

// 欧文モールス符号の通知音(morse_code.cpp)。液晶を持たない端末の状態通知に使う。
// PCMは実行時に合成してi2s_speakerへ書き込む。合成中はradio_streamを一時停止させる
// (i2s_speakerは単一プロデューサ前提のため)。
inline constexpr uint32_t kMorseSampleRateHz = 24000;
// サイドトーン周波数。kMorseSampleRateHzを割り切る値にすると1周期が整数サンプルに
// なり、波形テーブルをループさせても継ぎ目のクリックが出ない(24000/750=32)。
inline constexpr uint32_t kMorseToneHz = 800;
// 速度(words per minute)。1単位(短点)の長さ = 1200 / WPM ミリ秒。20WPMで60ms。
inline constexpr uint32_t kMorseWpm = 30;
// 各トーン要素の頭と尻に掛けるraised-cosineのアタック/リリース長。キークリック抑制。
inline constexpr uint32_t kMorseRampMs = 5;
// 合成PCMのint16ピーク振幅(フルスケール32767に対し余裕を持たせる)。ES8311の
// ハードウェアボリューム(kSpeakerVolume)とは別。実機で聴きながら調整すること。
inline constexpr int16_t kMorsePeakAmplitude = 13000;
// i2sSpeakerEnqueue()1回あたりのステレオフレーム数(256フレーム=1KB、
// kPcmWriterChunkBytesと同じ粒度)。
inline constexpr size_t kMorseChunkFrames = 256;
// i2sSpeakerFlush()で旧音声(ラジオ)のPCM末尾を捨て切るのを待つ時間。リング満杯でも
// kPcmRingBufferBytes分(24kHzステレオで約170ms)なので、それを上回る値にする。
// この待ちのあとにi2sSpeakerResume()してモールスのenqueueを始める。
inline constexpr uint32_t kMorseFlushSettleMs = 100;
// 最終enqueueのあと、末尾が実際に鳴り終えるのを待つ時間(enqueueはバッファ投入で戻る)。
inline constexpr uint32_t kMorseTailDrainMs = 200;
// 1回のmorsePlay()で受け付ける最大文字数(終端NUL含む)。超過分は切り捨てる。
inline constexpr size_t kMorseMaxTextLen = 24;
// 再生リクエストキューの段数。溢れたリクエストは捨てる。
inline constexpr size_t kMorseQueueDepth = 2;
inline constexpr uint32_t kMorseTaskStackBytes = 3072;
inline constexpr int kMorseTaskPriority = 1;
inline constexpr int kMorseTaskCore = 1;

// 保存済みのチャンネルが無い(初回起動)場合、および保存値が使えない場合に再生する
// チャンネルのインデックス(直接選局コードではなく kStations
// の添字)。0(=リストの先頭 =
// NHKの1局目、認証不要で常に再生可)なら局一覧がどう変わっても必ず有効。
inline constexpr int kBootChannelIndex = 0;

// MP3プレーヤー(mp3_player.cpp)。DAISYの音声ファイルをSDカードから順に再生する。
// SDスロットの配線はCardputer ADV(Cardputerと同じ): SCK=40, MISO=39, MOSI=14,
// CS=12。
inline constexpr int kSdSckPin = 40;
inline constexpr int kSdMisoPin = 39;
inline constexpr int kSdMosiPin = 14;
inline constexpr int kSdCsPin = 12;
inline constexpr uint32_t kSdSpiHz = 25000000;

// 再生対象のDAISY 2.02図書ディレクトリ(SDルートからの絶対パス)とそのncc.html。
// ncc.htmlに現れる順にSMILをたどり、各SMILの<audio>(src/clip-begin/clip-end)を
// 文書順に再生する。
inline constexpr char kBookDirectory[] = "/B4701R04540790";
inline constexpr char kNccFile[] = "ncc.html";

// シーク時にクリップ開始位置よりこれだけ手前からデコードを始め、開始位置までのPCMは捨てる。
// Layer IIIのbit
// reservoir(最大511バイト。32kbpsで約5フレーム=約130ms)が前フレームを
// 参照するため、途中から始めた直後の数フレームは正しくデコードできない。
inline constexpr uint32_t kMp3SeekPreRollMs = 160;

// 同一ファイル内で次クリップの開始が現在のクリップ終端よりこの範囲だけ先なら、シークせず
// デコードを続けて間のPCMを捨てる(シークより速く、フラッシュも要らない)。
inline constexpr uint32_t kMp3SkipThroughMs = 500;

// SDから1回に読んでデコーダへ渡すバイト数。i2sSpeakerEnqueue()がリング満杯でブロック
// するので、読み出しのペースはそれで自然に律速される。
inline constexpr size_t kMp3ReadChunkBytes = 1024;

// プレーヤータスク。デコーダはcore0(生産側)、I2S書き込みはcore1。
// 起動直後は morsePlay() のブートcueが鳴り終えるのを待ってから再生を始める
// (i2s_speakerは単一プロデューサ前提)。
inline constexpr uint32_t kMp3TaskStackBytes = 10240;
inline constexpr int kMp3TaskPriority = 1;
inline constexpr int kMp3TaskCore = 0;
inline constexpr uint32_t kMp3StartDelayMs = 1500;
inline constexpr size_t kMp3CommandQueueDepth = 4;

// しおりを保存しておく本の数(直近何冊分か)。NVSの1blob(1ページ=約2KB)に収まる範囲。
inline constexpr size_t kBookmarkMax = 100;

// TCA8418のキー番号(KeyEvent.key)。
inline constexpr uint8_t kKeyPrevHeading = 3;   // 前の見出しの先頭へ
inline constexpr uint8_t kKeyPrevPhrase = 4;    // 前のフレーズ(クリップ)へ
inline constexpr uint8_t kKeyHeadingStart = 7;  // 今の見出しの先頭へ戻る
inline constexpr uint8_t kKeyPlayStop = 8;      // 再生⇔一時停止
inline constexpr uint8_t kKeySlower = 15;       // 再生速度を1段遅く
inline constexpr uint8_t kKeyFaster = 16;       // 再生速度を1段速く
inline constexpr uint8_t kKeyNextHeading = 13;  // 次の見出しの先頭へ
inline constexpr uint8_t kKeyNextPhrase = 14;   // 次のフレーズ(クリップ)へ
inline constexpr uint8_t kKeyLevelUp = 17;      // 見出し移動の最深レベルを浅く(H6→H1方向)
inline constexpr uint8_t kKeyLevelDown = 18;    // 見出し移動の最深レベルを深く(H1→H6方向)
inline constexpr uint8_t kKeyVolumeDown = 11;   // 音量を1段下げる
inline constexpr uint8_t kKeyVolumeUp = 12;     // 音量を1段上げる

// 見出し移動の対象はH1〜この値のレベル。6なら全見出し。
inline constexpr uint8_t kHeadingLevelMin = 1;
inline constexpr uint8_t kHeadingLevelMax = 6;
inline constexpr uint8_t kHeadingLevelDefault = 6;

// 再生速度の段階(倍率)。Slower/Fasterキーで1段ずつ動く。音程を保ったまま話速だけを
// 変える(Sonic)。1.0付近は細かく、速い側は聞き慣れた人向けに3倍まで。
inline constexpr float kPlaybackSpeeds[] = {0.5f, 0.6f,  0.7f, 0.8f,  0.9f, 1.0f, 1.1f,
                                            1.2f, 1.35f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
inline constexpr size_t kPlaybackSpeedCount = sizeof(kPlaybackSpeeds) / sizeof(kPlaybackSpeeds[0]);
inline constexpr size_t kPlaybackSpeedDefault = 5;  // 1.0倍
static_assert(kPlaybackSpeedDefault < kPlaybackSpeedCount, "default speed index out of range");

}  // namespace config
