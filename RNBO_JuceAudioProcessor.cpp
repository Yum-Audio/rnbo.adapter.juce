/*
  ==============================================================================

	This file was auto-generated by the Introjucer!

	It contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "RNBO_JuceAudioProcessor.h"
#include "RNBO_JuceAudioProcessorEditor.h"
#include "RNBO_JuceAudioProcessorUtils.h"
#include "RNBO_Presets.h"
#include <readerwriterqueue/readerwriterqueue.h>
#include <iostream>
#include <sstream>
#include <algorithm>

//TODO get rid of this
using namespace juce;

namespace RNBO {

//RNBO parameter ID's can be too long for some hosts, so we hash the ID and render a string from the hash instead
juce::ParameterID paramIdForRNBOParam(RNBO::CoreObject& rnboObject, RNBO::ParameterIndex index, int versionHint) {
	RNBO::MessageTag t = RNBO::TAG(rnboObject.getParameterId(index));
	std::stringstream s;
	s << std::string("hashed_0x") << std::hex << std::setfill('0') << std::setw(8) << t;
	return juce::ParameterID(s.str(), versionHint);
}

//==============================================================================


JuceAudioProcessor::BusesProperties JuceAudioProcessor::makeBusesPropertiesForRNBOObject(RNBO::CoreObject &object, const nlohmann::json& patcher_desc)
{
	auto addBusses = [](bool isInput, const std::string& defaultName, const nlohmann::json &iolet, BusesProperties& bp) -> bool {
		if (!iolet.is_array()) {
			return false;
		}

		//name, count
		std::map<std::string, int> busses;
		for (auto i: iolet) {
			if (!(i.contains("index") && i.contains("tag") && i.contains("type") && i["type"].get<std::string>() == "signal")) {
				continue;
			}
			std::string busName = defaultName;

			//lookup meta data
			if (i.contains("meta") && i["meta"].is_object()) {
				nlohmann::json meta = i["meta"];
				if (meta.contains("bus") && meta["bus"].is_string()) {
					busName = meta["bus"].get<std::string>();
				}
			}
			auto it = busses.find(busName);
			if (it != busses.end()) {
				it->second++;
			} else {
				busses.insert({busName, 1});
			}
		}

		for (auto& kv: busses) {
			String busName(kv.first);
			int count = kv.second;
			bp.addBus(isInput, busName, juce::AudioChannelSet::canonicalChannelSet(count), true);
		}

		return true;
	};

	{
		//add bus properties, if we fail, simply create Input/Output with all the channels
		auto bp = BusesProperties();
		try {
			if (
					patcher_desc.contains("inlets") && addBusses(true, "Input", patcher_desc["inlets"], bp) &&
					patcher_desc.contains("outlets") && addBusses(false, "Output", patcher_desc["outlets"], bp)
				 ) {
				return bp;
			}
		} catch (std::exception& e) {
			std::cerr << "exception processing inlet json" << std::endl;
		}
	}

	{
		auto bp = BusesProperties();
		if (object.getNumInputChannels() > 0)
			bp.addBus(true, "Input", juce::AudioChannelSet::canonicalChannelSet((int) object.getNumInputChannels()), true);
		if (object.getNumOutputChannels() > 0)
			bp.addBus(false, "Output", juce::AudioChannelSet::canonicalChannelSet((int) object.getNumOutputChannels()), true);
		return bp;
	}
}

JuceAudioProcessor::JuceAudioProcessor(
		const nlohmann::json& patcher_desc,
		const nlohmann::json& presets,
		const RNBO::BinaryData& data,
		JuceAudioParameterFactory* paramFactory,
        const BusesProperties& busesProperties
		)
	: CoreObjectHolder(this)
	, AudioProcessor(
#ifdef PLUGIN_BUSES_PROPERTIES
		PLUGIN_BUSES_PROPERTIES
#else
        busesProperties 
        // JuceAudioProcessor::makeBusesPropertiesForRNBOObject(_rnboObject, patcher_desc)
#endif
	)
	// , Thread("fileLoadAndDealloc") //background thread - disabled on Yum Audio repo, not needed on our plugins
	, _currentPresetIdx(0)
{
	_dataRefCleanupQueue = make_unique<moodycamel::ReaderWriterQueue<char *, 32>>(static_cast<size_t>(32));
	_dataRefLoadQueue = make_unique<moodycamel::ReaderWriterQueue<std::pair<juce::String, juce::File>, 32>>(static_cast<size_t>(32));

	_formatManager.registerBasicFormats();

	//create default param factory if one isn't passed in
	std::unique_ptr<JuceAudioParameterFactory> fact;
	if (paramFactory == nullptr) {
		fact = std::make_unique<JuceAudioParameterFactory>(patcher_desc);
		paramFactory = fact.get();
	}

	//get parameter desc
	nlohmann::json paramdesc;
	{
		const std::string key = "parameters";
		if (patcher_desc.contains(key) && patcher_desc[key].is_array()) {
			paramdesc = patcher_desc[key];
		}
	}

	int juceIndex = 0;
	for (ParameterIndex i = 0; i < _rnboObject.getNumParameters(); i++) {
		//create can return nullptr
		juce::AudioProcessorParameter * p = paramFactory->create(_rnboObject, i);
		if (p) {
			_rnboParamIndexToJuceParamIndex.insert({i, juceIndex++});
			addParameter(p);
#if RNBO_JUCE_PARAM_DEFAULT_NOTIFY
			bool notify = true;
#else
			bool notify = false;
#endif
			//see if we have the notify meta
			if (paramdesc.size() > i) {
				const std::string meta = "meta";
				const std::string key = "notify";
				auto desc = paramdesc[i];
				if (desc.contains(meta) && desc[meta].is_object() && desc[meta].contains(key) && desc[meta][key].is_boolean()) {
					notify = desc[meta][key].get<bool>();
				}
			}

			if (notify) {
				_notifyingParameters.insert(i);
			}
		}
	}

	//save initial preset
	_initialPreset = _rnboObject.getPresetSync();

	//Read presets
	try  {
		if (!(presets.is_null() || presets.empty())) {
			std::string s = presets.dump();
			_presetList = std::make_unique<PresetList>(s);
		}
	} catch (const std::exception& e) {
		std::cerr << "exception reading presets json " << e.what() << std::endl;
	}

	//datarefs
	try  {
		const std::string key("externalDataRefs");
		const std::string fkey("file");
		const std::string idkey("id");
		if (patcher_desc.contains(key) && patcher_desc[key].is_array()) {
			for (auto dep: patcher_desc[key]) {
				if (dep.contains(fkey) && dep.contains(idkey)) {
					const std::string fileName = dep[fkey].get<std::string>();
					const std::string id = dep[idkey].get<std::string>();
					BinaryDataEntry entry;
					if (data.get(fileName, entry) && entry.length() > 0) {
						std::unique_ptr<MemoryInputStream> memoryStream = make_unique<MemoryInputStream>(entry.data(), entry.length(), false);
						std::unique_ptr<juce::AudioFormatReader> reader(_formatManager.createReaderFor(std::move(memoryStream)));
						loadDataRef(id, fileName, std::move(reader));
					}
				}
			}
		}
	} catch (const std::exception& e) {
		std::cerr << "exception reading/loading externalDataRefs" << e.what() << std::endl;
	}

	//start audio loading/dealloc thread
	// startThread();
}

JuceAudioProcessor::~JuceAudioProcessor()
{
	//stop audio loading/dealloc thread
	// stopThread(200);
}

#ifdef JUCE_STATIC_PLUGIN
	extern "C" const char * JuceStatic_Plugin_Name();
#endif

//==============================================================================
const juce::String JuceAudioProcessor::getName() const
{
#ifndef JUCE_STATIC_PLUGIN
#ifdef JucePlugin_Name
	return JucePlugin_Name;
#else
		// Perhaps we should get the name from the generated code via some call?
	return "RNBO Plugin";
#endif
#else
	return juce::String(JuceStatic_Plugin_Name());
#endif
}

void JuceAudioProcessor::handleParameterEvent(const ParameterEvent& event)
{
	// Engine might have parameters than aren't exposed to JUCE
	// so we need to filter out any parameter events that are not in our _rnboParamIndexToJuceParamIndex
	auto it = _rnboParamIndexToJuceParamIndex.find(event.getIndex());
	if (it != _rnboParamIndexToJuceParamIndex.end()) {
		// we need to normalize the parameter value
		ParameterValue normalizedValue = _rnboObject.convertToNormalizedParameterValue(event.getIndex(), event.getValue());
		const auto param = getParameters()[it->second];
		if (_isInStartup) {
			param->setValue((float)normalizedValue);
		}
		else if (_isSettingPresetAsync || _notifyingParameters.count(event.getIndex()) != 0) {
			param->beginChangeGesture();
			param->setValueNotifyingHost((float)normalizedValue);
			param->endChangeGesture();
		}
	}
}

void JuceAudioProcessor::handleStartupEvent(const RNBO::StartupEvent& event)
{
	_isInStartup = event.getType() == RNBO::StartupEvent::Begin;
}

void JuceAudioProcessor::handlePresetEvent(const RNBO::PresetEvent& event)
{
	if (event.getType() == RNBO::PresetEvent::SettingBegin) {
		_isSettingPresetAsync = true;
	}
	else if (event.getType() == RNBO::PresetEvent::SettingEnd) {
		_isSettingPresetAsync = false;
	}
}
void JuceAudioProcessor::handleMessageEvent(const RNBO::MessageEvent& event) {
	static MessageTag setlatency = RNBO::TAG("setlatency");
	if (event.getTag() == setlatency) {
		if (event.getType() == RNBO::MessageEvent::Type::Number) {
			setLatencySamples(static_cast<int>(event.getNumValue()));
		}
	} else {
		RNBO::EventHandler::handleMessageEvent(event);
	}
}

//background thread - disabled on Yum Audio repo, not needed on our plugins
// void JuceAudioProcessor::run() {
// 	while (! threadShouldExit())
// 	{
// 		std::pair<juce::String, juce::File> load;
// 		while (_dataRefLoadQueue->try_dequeue(load)) {
// 			auto refName = load.first;
// 			auto file = load.second;
// 			std::unique_ptr<juce::AudioFormatReader> reader (_formatManager.createReaderFor (file));
// 			loadDataRef(refName, file.getFileName(), std::move(reader));
// 		}

// 		//cleanup any buffers we need to dealloc
// 		char * b;
// 		while (_dataRefCleanupQueue->try_dequeue(b)) {
// 			delete [] b;
// 		}

// 		wait (500);
// 	}
// }

void JuceAudioProcessor::addDataRefListener(juce::MessageListener * listener) {
	std::lock_guard g(_loadedDataRefsMutex);
	_dataRefListener = listener;
}

juce::String JuceAudioProcessor::loadedDataRefFile(const juce::String refName) {
	std::lock_guard g(_loadedDataRefsMutex);
	juce::String file;
	auto it = _loadedDataRefs.find(refName);
	if (it != _loadedDataRefs.end()) {
		file = it->second;
	}
	return file;
}

void JuceAudioProcessor::loadDataRef(const juce::String refName, const juce::File file) {
	jassertfalse; //not used on Yum Audio repo and derived products. If you end up calling in here, make sure to re-implement the Thread (just uncomment all the parts)
    _dataRefLoadQueue->enqueue(std::make_pair(refName, file));
	// notify();//wakeup
}

void JuceAudioProcessor::loadDataRef(const juce::String refName, const juce::String fileName, std::unique_ptr<juce::AudioFormatReader> reader) {
	try {
		if (reader)
		{
			std::shared_ptr<juce::AudioSampleBuffer> buffer = std::make_shared<juce::AudioSampleBuffer>();
			auto chans = static_cast<int>(reader->numChannels);
			auto length = static_cast<int>(reader->lengthInSamples);
			buffer->setSize (chans, length);
			if (reader->read(buffer.get(), 0, length, 0, true, true)) {
				size_t samps = static_cast<size_t>(reader->numChannels * reader->lengthInSamples);

				RNBO::Float32AudioBuffer bufferType(reader->numChannels, reader->sampleRate);

				float * data = new float[samps];
				using Format = AudioData::Format<AudioData::Float32, AudioData::NativeEndian>;

				juce::AudioData::interleaveSamples(
						AudioData::NonInterleavedSource<Format> { buffer->getArrayOfReadPointers(), chans },
						AudioData::InterleavedDest<Format> { data, chans },
						length
						);

				{
					std::lock_guard g(_loadedDataRefsMutex);
					_rnboObject.setExternalData(
							refName.toRawUTF8(),
							reinterpret_cast<char *>(data),
							samps * sizeof(float),
							bufferType,
							[this](RNBO::ExternalDataId, char* d) {
								//hold onto shared_ptr until rnbo stops using it
                                if (_dataRefCleanupQueue)
								    _dataRefCleanupQueue->enqueue(d);
							}
					);
					_loadedDataRefs.insert({refName, fileName});
					if (_dataRefListener) {
						_dataRefListener->postMessage(new DataRefUpdatedMessage(refName, fileName));
					}
				}
			}
		}
	} catch (...) {
	}
}

void JuceAudioProcessor::handleAsyncUpdate()
{
	drainEvents();
}

bool JuceAudioProcessor::acceptsMidi() const
{
	return _rnboObject.getNumMidiInputPorts() > 0;
}

bool JuceAudioProcessor::producesMidi() const
{
	return _rnboObject.getNumMidiOutputPorts() > 0;
}

bool JuceAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
	return true;
#else
	return false;
#endif
}


bool JuceAudioProcessor::silenceInProducesSilenceOut() const
{
	return false;
}

double JuceAudioProcessor::getTailLengthSeconds() const
{
	return 0.0;
}

int JuceAudioProcessor::getNumPrograms()
{
	// NB: some hosts don't cope very well if you tell them there are 0
	// so this should be at least 1, even if you're not really implementing programs.
	if (!_presetList) {
		return 1;
	} else {
		return (int) _presetList->size() + 1; //add "initial"
	}
}

int JuceAudioProcessor::getCurrentProgram()
{
	return _currentPresetIdx;
}

void JuceAudioProcessor::setCurrentProgram (int index)
{
	if (_presetList) {
		_currentPresetIdx = index;
		UniquePresetPtr preset;
		if (index == 0 && _initialPreset) {
			preset = make_unique<RNBO::Preset>();
			RNBO::copyPreset(*_initialPreset, *preset);
		} else if (index > 0) {
			preset = _presetList->presetAtIndex(static_cast<size_t>(index - 1));
		}
		if (preset) {
			_rnboObject.setPreset(std::move(preset));
		}
	}
}

const juce::String JuceAudioProcessor::getProgramName (int index)
{
    if (!_presetList || index < 0) {
        return juce::String();
		} else if (index == 0) {
				return juce::String("inital");
    } else {
        std::string name = _presetList->presetNameAtIndex((size_t)index - 1);
        return juce::String(name);
    }
}

void JuceAudioProcessor::changeProgramName (int index, const String& newName)
{
	RNBO_UNUSED(index)
	RNBO_UNUSED(newName)
	// Can't do it
}

//==============================================================================

void JuceAudioProcessor::prepareToPlay(double sampleRate, int estimatedSamplesPerBlock)
{
	_rnboObject.prepareToProcess(sampleRate, static_cast<Index>(estimatedSamplesPerBlock));
}

void JuceAudioProcessor::releaseResources()
{
}

bool JuceAudioProcessor::isBusesLayoutSupported (const BusesLayout& /*layouts*/) const
{
	#if JucePlugin_IsMidiEffect
		return true;
	#endif
	//TODO count the number of main inputs/outputs and make sure they match?
	// return static_cast<Index>(layouts.getMainInputChannels()) == _rnboObject.getNumInputChannels() && static_cast<Index>(layouts.getMainOutputChannels()) == _rnboObject.getNumOutputChannels();
	return true;
}

void JuceAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	auto samples = static_cast<Index>(buffer.getNumSamples());
	auto tc = preProcess(midiMessages);
	_rnboObject.process(
			buffer.getArrayOfReadPointers(), static_cast<Index>(buffer.getNumChannels()),
			buffer.getArrayOfWritePointers(), static_cast<Index>(buffer.getNumChannels()),
			samples,
			&_midiInput, &_midiOutput
			);
	postProcess(tc, midiMessages);
}

void JuceAudioProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midiMessages)
{
	auto samples = static_cast<Index>(buffer.getNumSamples());
	auto tc = preProcess(midiMessages);
	_rnboObject.process(
			buffer.getArrayOfReadPointers(), static_cast<Index>(buffer.getNumChannels()),
			buffer.getArrayOfWritePointers(), static_cast<Index>(buffer.getNumChannels()),
			samples,
			&_midiInput, &_midiOutput
			);
	postProcess(tc, midiMessages);
}

TimeConverter JuceAudioProcessor::preProcess(juce::MidiBuffer& midiMessages) {
	RNBO::MillisecondTime time = _rnboObject.getCurrentTime();

	//transport
	{
		AudioPlayHead* playhead = getPlayHead();
		if (playhead) {
			auto info = playhead->getPosition();
			if (info) {
				auto bpm = info->getBpm();
				if (bpm && *bpm != _lastBPM) {
					_lastBPM = *bpm;
					RNBO::TempoEvent event(time, _lastBPM);
					_rnboObject.scheduleEvent(event);
				}

				auto timesig = info->getTimeSignature();
				if (timesig && (timesig->numerator != _lastTimeSigNumerator || timesig->denominator != _lastTimeSigDenominator)) {
					_lastTimeSigNumerator = timesig->numerator;
					_lastTimeSigDenominator = timesig->denominator;
					RNBO::TimeSignatureEvent event(time, _lastTimeSigNumerator, _lastTimeSigDenominator);
					_rnboObject.scheduleEvent(event);
				}

				auto ppqPos = info->getPpqPosition();
				if (ppqPos && *ppqPos != _lastPpqPosition) {
					_lastPpqPosition = *ppqPos;
					RNBO::BeatTimeEvent event(time, _lastPpqPosition);
					_rnboObject.scheduleEvent(event);
				}

				auto playing = info->getIsPlaying();
				if (playing != _lastIsPlaying) {
					_lastIsPlaying = playing;
					RNBO::TransportEvent event(time, _lastIsPlaying ? RNBO::TransportState::RUNNING : RNBO::TransportState::STOPPED);
					_rnboObject.scheduleEvent(event);
				}
			}
		}
	}

	TimeConverter timeConverter(_rnboObject.getSampleRate(), time);

	// fill midi input
	_midiInput.clear();  // make sure midi input starts clear
	for (auto m: midiMessages)
	{
		MillisecondTime t = timeConverter.convertSampleOffsetToMilliseconds(m.samplePosition);
		//data might be more than 3 bytes long so we chunk it up
		const Index bytes = (Index)m.numBytes;
		for (Index i = 0; i < bytes; i += 3) {
			_midiInput.addEvent(MidiEvent(t, 0, m.data + i, std::min((Index)3, bytes - i)));
		}
	}
	midiMessages.clear();		// clear the input that we consumed above so juce doesn't get confused
	return timeConverter;
}

void JuceAudioProcessor::postProcess(TimeConverter& timeConverter, juce::MidiBuffer& midiMessages) {
	// consume midi output
	if (!_midiOutput.empty()) {
		for (const auto& ev: _midiOutput) {
			int sampleNumber = static_cast<int>(timeConverter.convertMillisecondsToSampleOffset(ev.getTime()));
			auto midiMessage = MidiMessage(ev.getData(), (int)ev.getLength());
			midiMessages.addEvent(midiMessage, sampleNumber);
		}
		_midiOutput.clear();
	}
}

//==============================================================================
bool JuceAudioProcessor::hasEditor() const
{
	return true;
}

AudioProcessorEditor* JuceAudioProcessor::createEditor()
{
	return new RNBOAudioProcessorEditor(this, _rnboObject);
}

//==============================================================================
void JuceAudioProcessor::getStateInformation (MemoryBlock& destData)
{
	auto rnboPreset = _rnboObject.getPresetSync();
	auto rnboPresetStr = RNBO::convertPresetToJSON(*rnboPreset);
	MemoryOutputStream stream(destData, false);
	stream.writeString(rnboPresetStr);
}

void JuceAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	String rnboPresetStr = String::createStringFromData (data, sizeInBytes);
	auto rnboPreset = RNBO::convertJSONToPreset(rnboPresetStr.toStdString());
	_rnboObject.setPresetSync(std::move(rnboPreset));
	// now let us get all parameter updates that were triggered by the preset update immediately
	drainEvents();
}

void JuceAudioProcessor::eventsAvailable()
{
	this->triggerAsyncUpdate();
}

JuceAudioParameterFactory::JuceAudioParameterFactory(
		const nlohmann::json& patcherdesc
		)
	: _patcherDesc(patcherdesc)
{
	try {
		const std::string key = "parameters";
		if (_patcherDesc.contains(key) && _patcherDesc[key].is_array()) {
			for (auto p: _patcherDesc[key]) {
				if (
						p.is_object() &&
						p.contains("index") && p["index"].is_number() &&
						p.contains("meta") && p["meta"].is_object()
					 )
				{
					ParameterIndex id = static_cast<ParameterIndex>(p["index"].get<int>());
					_paramMeta[id] = p["meta"];
				}
			}
		}
	} catch (std::exception& e) {
		std::cerr << "exception reading parameters json " << e.what() << std::endl;
	}
}

AudioProcessorParameter* JuceAudioParameterFactory::create(RNBO::CoreObject& rnboObject, ParameterIndex index) {
	ParameterInfo info;
	rnboObject.getParameterInfo(index, &info);

	if (!info.visible) {
		return nullptr;
	}

	//get meta
	nlohmann::json meta;

	auto it = _paramMeta.find(index);
	int versionHint = 1;
	if (it != _paramMeta.end()) {
		meta = it->second;

		//find version hint, if we have one
		const std::string vkey = "versionhint";
		if (meta.contains(vkey) && meta[vkey].is_number())
		{
			versionHint = meta[vkey].get<int>();
		}
	}
	return create(rnboObject, index, info, versionHint, meta);
}

juce::AudioProcessorParameter* JuceAudioParameterFactory::create(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta) {
	if (info.enumValues && info.steps > 0) {
		return createEnum(rnboObject, index, info, versionHint, meta);
	} else {
		return createFloat(rnboObject, index, info, versionHint, meta);
	}
}

AudioProcessorParameter* JuceAudioParameterFactory::createEnum(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta) {
	return new EnumParameter(index, info, rnboObject, versionHint, automate(meta));
}

AudioProcessorParameter* JuceAudioParameterFactory::createFloat(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta) {
	return new FloatParameter(index, info, rnboObject, versionHint, automate(meta));
}

bool JuceAudioParameterFactory::automate(const nlohmann::json& meta) {
	const std::string key = "automate";
	if (meta.contains(key) && meta[key].is_boolean())
		return meta[key].get<bool>();
	return true;
}


} // namespace RNBO

#ifdef RNBO_INCLUDE_DESCRIPTION_FILE
#include <rnbo_description.h>
#endif

// optionally disable createPluginFilter so you can implement your own with a subclass of RNBO::JuceAudioProcessor
#ifndef RNBO_JUCE_NO_CREATE_PLUGIN_FILTER
//==============================================================================
// This creates new instances of the plugin..
AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	nlohmann::json patcher_desc, presets;


#ifdef RNBO_BINARY_DATA_STORAGE_NAME
	extern RNBO::BinaryDataImpl::Storage RNBO_BINARY_DATA_STORAGE_NAME;
	RNBO::BinaryDataImpl::Storage dataStorage = RNBO_BINARY_DATA_STORAGE_NAME;
#else
	RNBO::BinaryDataImpl::Storage dataStorage;
#endif
	RNBO::BinaryDataImpl data(dataStorage);

#ifdef RNBO_INCLUDE_DESCRIPTION_FILE
	patcher_desc = RNBO::patcher_description;
	presets = RNBO::patcher_presets;
#endif

	RNBO::JuceAudioParameterFactory factory(patcher_desc);

	return new RNBO::JuceAudioProcessor(patcher_desc, presets, data, &factory);
}
#endif
