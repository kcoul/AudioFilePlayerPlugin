/*
    PluginProcessor.cpp
    Copyright (C) 2017 Jonathon Racz, ROLI Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

AudioFilePlayerProcessor::AudioFilePlayerProcessor() :
    AudioProcessor(BusesProperties().withOutput("Output", AudioChannelSet::stereo())),
    thumbnailCache(1),
    readAheadThread("transport read ahead")
{
    formatManager.registerBasicFormats();
    readAheadThread.startThread();
}

AudioFilePlayerProcessor::~AudioFilePlayerProcessor()
{
    transportSource.setSource(nullptr);
}

const String AudioFilePlayerProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioFilePlayerProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioFilePlayerProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

double AudioFilePlayerProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioFilePlayerProcessor::getNumPrograms()
{
    return 1;
}

int AudioFilePlayerProcessor::getCurrentProgram()
{
    return 0;
}

void AudioFilePlayerProcessor::setCurrentProgram(int index)
{
}

const String AudioFilePlayerProcessor::getProgramName(int index)
{
    return String();
}

void AudioFilePlayerProcessor::changeProgramName(int index, const String& newName)
{
}

void AudioFilePlayerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    transportSource.prepareToPlay(samplesPerBlock, sampleRate);
}

void AudioFilePlayerProcessor::releaseResources()
{
    transportSource.releaseResources();
}

void AudioFilePlayerProcessor::loadMIDIFile(juce::File file)
{
    MIDIFile.clear();
    
    juce::FileInputStream stream(file);
    MIDIFile.readFrom(stream);

    /** This function call means that the MIDI file is going to be played with the
        original tempo and signature.
    */
    MIDIFile.convertTimestampTicksToSeconds();
    
    numTracks.store(MIDIFile.getNumTracks());
    currentTrack.store(0);
    trackHasChanged = false;
}

void AudioFilePlayerProcessor::lumiMIDIEvent(const void* message, size_t size)
{
    if (lumi)
        lumi->sendMessage(message, size);
}

void AudioFilePlayerProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    
    for(int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    transportSource.getNextAudioBlock(AudioSourceChannelInfo(buffer));
    
    const juce::ScopedTryLock myScopedLock(processLock);

    if (myScopedLock.isLocked())
    {
        if (numTracks.load() > 0)
        {
            const juce::MidiMessageSequence *theSequence = MIDIFile.getTrack(currentTrack.load());

            auto startTime = 0;
            auto endTime = theSequence->getEndTime();
            auto sampleLength = 1.0 / getSampleRate();

            // If the transport bar position has been moved by the user or because of looping
            if (std::abs(startTime - nextStartTime) > sampleLength && nextStartTime > 0.0)
                sendAllNotesOff(midiMessages);

            nextStartTime = endTime;

            // If the MIDI file doesn't contain any event anymore
            if (isPlayingSomething && startTime >= theSequence->getEndTime())
                sendAllNotesOff(midiMessages);

            else
            {
                // Called when the user changes the track during playback
                if (trackHasChanged)
                {
                    trackHasChanged = false;
                    sendAllNotesOff(midiMessages);
                }
                                
                // Iterating through the MIDI file contents and trying to find an event that
                // needs to be called in the current time frame
                
                for (auto i = 0; i < theSequence->getNumEvents(); i++)
                {
                    juce::MidiMessageSequence::MidiEventHolder *event = theSequence->getEventPointer(i);

                    if (event->message.getTimeStamp() >= startTime && event->message.getTimeStamp() < endTime)
                    {
                        //auto samplePosition = juce::roundToInt((event->message.getTimeStamp() - startTime) * getSampleRate());
                        //midiMessages.addEvent(event->message, samplePosition);

                        lumiMIDIEvent((void*)event->message.getRawData(), event->message.getRawDataSize());
                        
                        isPlayingSomething = true;
                    }
                }
                 
            }
        }
    }
    else
    {
        // If we have just opened a MIDI file with no content
        if (isPlayingSomething)
            sendAllNotesOff(midiMessages);
    }
}

void AudioFilePlayerProcessor::sendAllNotesOff(juce::MidiBuffer& midiMessages)
{
    for (auto i = 1; i <= 16; i++)
    {
        midiMessages.addEvent(juce::MidiMessage::allNotesOff(i), 0);
        midiMessages.addEvent(juce::MidiMessage::allSoundOff(i), 0);
        midiMessages.addEvent(juce::MidiMessage::allControllersOff(i), 0);
    }

    isPlayingSomething = false;
}

bool AudioFilePlayerProcessor::hasEditor() const
{
    return true;
}

AudioProcessorEditor* AudioFilePlayerProcessor::createEditor()
{
    return new AudioFilePlayerEditor(*this);
}

void AudioFilePlayerProcessor::getStateInformation(MemoryBlock& destData)
{
    XmlElement xml("plugin-settings");

    xml.setAttribute("audiofile", currentlyLoadedFile.getFullPathName());

    copyXmlToBinary(xml, destData);
}

void AudioFilePlayerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr)
    {
        if (xmlState->hasTagName("plugin-settings"))
        {
            currentlyLoadedFile = File::createFileWithoutCheckingPath(xmlState->getStringAttribute("audiofile"));
            if (currentlyLoadedFile.existsAsFile())
            {
                loadAudioFileIntoTransport(currentlyLoadedFile);
            }
        }
    }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioFilePlayerProcessor();
}

void AudioFilePlayerProcessor::loadAudioFileIntoTransport(const File& audioFile)
{
    // unload the previous file source and delete it..
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource = nullptr;

    AudioFormatReader* reader = formatManager.createReaderFor(audioFile);
    currentlyLoadedFile = audioFile;

    if (reader != nullptr)
    {
        currentAudioFileSource = std::make_unique<AudioFormatReaderSource>(reader, true);

        // ..and plug it into our transport source
        transportSource.setSource(
            currentAudioFileSource.get(),
            32768,                   // tells it to buffer this many samples ahead
            &readAheadThread,        // this is the background thread to use for reading-ahead
            reader->sampleRate);     // allows for sample rate correction
    }
}
