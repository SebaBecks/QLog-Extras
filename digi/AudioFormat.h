#ifndef QLOG_DIGI_AUDIOFORMAT_H
#define QLOG_DIGI_AUDIOFORMAT_H

/* Sample layout of an audio buffer, independent of the Qt 5/6 QAudioFormat
   API differences. The audio classes translate it at their edge. */
namespace DigiAudio
{

enum class Sample
{
    UInt8,
    Int16,
    Int32,
    Float
};

inline int bytesPerSample(Sample format)
{
    switch ( format )
    {
    case Sample::UInt8:  return 1;
    case Sample::Int32:
    case Sample::Float:  return 4;
    default:             return 2;
    }
}

} // namespace DigiAudio

#endif // QLOG_DIGI_AUDIOFORMAT_H
