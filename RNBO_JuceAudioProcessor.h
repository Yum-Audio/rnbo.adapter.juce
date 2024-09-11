/*
 ==============================================================================

 This file was auto-generated by the Introjucer!

 It contains the basic framework code for a JUCE plugin processor.

 ==============================================================================
 */

#ifndef _RNBO_JUCEPLUGINPROCESSOR_H_
#define _RNBO_JUCEPLUGINPROCESSOR_H_

#include "RNBO.h"
#include "RNBO_BinaryData.h"
#include "RNBO_TimeConverter.h"
#include <unordered_map>
#include <vector>
#include <mutex>

#include <json/json.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace moodycamel {
template<typename T, size_t MAX_BLOCK_SIZE>
class ReaderWriterQueue;
}

namespace RNBO {
	juce::ParameterID paramIdForRNBOParam(RNBO::CoreObject& rnboObject, RNBO::ParameterIndex index, int versionHint);

	//holder class so we can construct the CoreObject before AudioProcessor
	class CoreObjectHolder {
	public:
		CoreObjectHolder(EventHandler* handler)
		: _rnboObject(handler)
		{}

		//==============================================================================
		RNBO::CoreObject& getRnboObject() { return _rnboObject; }
	protected:
		RNBO::CoreObject						_rnboObject;
	};

	//factory class for creating parameters, override to customize parameter creation
	class JuceAudioParameterFactory {
		public:
			JuceAudioParameterFactory(const nlohmann::json& patcherdesc);
			virtual ~JuceAudioParameterFactory() = default;

			//entrypoint, may return null
			juce::AudioProcessorParameter* create(RNBO::CoreObject& rnboObject, ParameterIndex index);

		protected:
			//overrideable entrypoint
			virtual juce::AudioProcessorParameter* create(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta);

			//called by create if appropriate
			virtual juce::AudioProcessorParameter* createEnum(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta);
			virtual juce::AudioProcessorParameter* createFloat(RNBO::CoreObject& rnboObject, ParameterIndex index, const ParameterInfo& info, int versionHint, const nlohmann::json& meta);

			bool automate(const nlohmann::json& meta);

			const nlohmann::json& _patcherDesc;
			std::unordered_map<RNBO::ParameterIndex, nlohmann::json> _paramMeta;
	};

	//==============================================================================
	/**
	 */
	class JuceAudioProcessor :
		public RNBO::EventHandler,
		public CoreObjectHolder,
		public juce::AudioProcessor,
		public juce::AsyncUpdater,
		private juce::Thread
	{
		using String = juce::String;
	public:
		//==============================================================================
		JuceAudioProcessor(
				const nlohmann::json& patcher_description,
				const nlohmann::json& presets,
				const RNBO::BinaryData& data,
				JuceAudioParameterFactory* paramFactory = nullptr,
                BusesProperties* busesProperties = nullptr
		);
		~JuceAudioProcessor() override;

		//==============================================================================
		void prepareToPlay (double sampleRate, int samplesPerBlock) override;
		void releaseResources() override;

		static BusesProperties makeBusesPropertiesForRNBOObject(RNBO::CoreObject &object, const nlohmann::json& patcher_description);

		bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

		void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
		void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

		//==============================================================================
		juce::AudioProcessorEditor* createEditor() override;
		bool hasEditor() const override;

		//==============================================================================
		const String getName() const override;

		bool acceptsMidi() const override;
		bool producesMidi() const override;
		bool isMidiEffect() const override;
		bool silenceInProducesSilenceOut() const override;
		double getTailLengthSeconds() const override;

		//==============================================================================
		int getNumPrograms() override;
		int getCurrentProgram() override;
		void setCurrentProgram (int index) override;
		const String getProgramName (int index) override;
		void changeProgramName (int index, const String& newName) override;

		//==============================================================================
		void getStateInformation (juce::MemoryBlock& destData) override;
		void setStateInformation (const void* data, int sizeInBytes) override;

		//==============================================================================

		void handleAsyncUpdate() override;
		void eventsAvailable() override;

		void handleParameterEvent(const RNBO::ParameterEvent& event) override;
		void handleStartupEvent(const RNBO::StartupEvent& event) override;
		void handlePresetEvent(const RNBO::PresetEvent& event) override;
		void handleMessageEvent(const RNBO::MessageEvent& event) override;

		//background thread
		void run() override;

		void addDataRefListener(juce::MessageListener * listener);

		juce::String loadedDataRefFile(const juce::String refName);
		void loadDataRef(const juce::String refName, const juce::File file);

	private:
		void loadDataRef(const juce::String refName, const juce::String fileName, std::unique_ptr<juce::AudioFormatReader> reader);

		TimeConverter preProcess(juce::MidiBuffer& midiMessages);
		void postProcess(TimeConverter& timeConverter, juce::MidiBuffer& midiMessages);

		class SyncEventHandler : public RNBO::EventHandler
		{
		public:
			SyncEventHandler(JuceAudioProcessor& owner)
			: _owner(owner)
			{}

			void eventsAvailable() override {}

			void handleParameterEvent(const RNBO::ParameterEvent& event) override;
			void handlePresetEvent(const RNBO::PresetEvent& event) override;

		private:
			bool				_isSettingPresetSync = false;
			JuceAudioProcessor& _owner;
		};

		//==============================================================================
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceAudioProcessor)

		RNBO::MidiEventList						_midiInput;
		RNBO::MidiEventList						_midiOutput;
		std::unique_ptr<RNBO::PresetList>	_presetList;
		SyncEventHandler						_syncEventHandler;
		RNBO::ParameterEventInterfaceUniquePtr	_syncParamInterface;
		int										_currentPresetIdx;
		bool									_isInStartup = false;
		bool									_isSettingPresetAsync = false;

		//rnbo might have some invisible parameters that aren't given to juce, so we map the rnbo index to the juce index
		std::unordered_map<RNBO::ParameterIndex, int> _rnboParamIndexToJuceParamIndex;

		//which parameters should cause host notifications when coming out of the core object
		std::set<RNBO::ParameterIndex> _notifyingParameters;

		//id -> file name
		std::unordered_map<juce::String, juce::String> _loadedDataRefs;
		std::mutex _loadedDataRefsMutex;


		double _lastBPM = -1.0;
		int _lastTimeSigNumerator = 0;
		int _lastTimeSigDenominator = 0;
		double _lastPpqPosition = -1.0;
		bool _lastIsPlaying = false;

    juce::AudioFormatManager _formatManager;
		juce::MessageListener * _dataRefListener = nullptr;

		std::unique_ptr<moodycamel::ReaderWriterQueue<char *, 32>> _dataRefCleanupQueue;
		std::unique_ptr<moodycamel::ReaderWriterQueue<std::pair<juce::String, juce::File>, 32>> _dataRefLoadQueue;
	};

	class DataRefUpdatedMessage : public juce::Message {
		public:
			DataRefUpdatedMessage(juce::String refName, juce::String fileName) :_refName(refName), _fileName(fileName) { }
			virtual ~DataRefUpdatedMessage() { }
			juce::String refName() const { return _refName; }
			juce::String fileName() const { return _fileName; }
		private:
			juce::String _refName;
			juce::String _fileName;
	};

	class FloatParameter : public juce::RangedAudioParameter
	{
		using String = juce::String;
	public:

		FloatParameter (ParameterIndex index, const ParameterInfo& info, CoreObject& rnboObject, int versionHint = 0, bool automatable = true)
		:
			juce::RangedAudioParameter(
					paramIdForRNBOParam(rnboObject, index, versionHint),
					String(rnboObject.getParameterName(index))
			)
		, _index(index)
		, _rnboObject(rnboObject)
		, _automatable(automatable)
		{

			if (info.unit) {
				_unitName = String(info.unit);
			}

			_name = String(info.displayName);
			if (_name.isEmpty()) {
				_name = String(_rnboObject.getParameterId(_index));
			}

			_defaultValue = static_cast<float>(_rnboObject.convertToNormalizedParameterValue(_index, info.initialValue));

			auto min = static_cast<float>(info.min);
			auto max = static_cast<float>(info.max);
			if (info.steps) {
				_normRange = juce::NormalisableRange<float>(min, max, 1.0f);
			} else {
				_normRange = juce::NormalisableRange<float>(min, max);
			}
		}

		float getValue() const override
		{
			// getValue wants the value between 0 and 1
			float normalizedValue = (float)_rnboObject.getParameterNormalized(_index);
			return normalizedValue;
		}

		void setValue (float newValue) override
		{
			jassert(newValue >= 0 && newValue <= 1.);	// should be getting normalized values
			float oldValue = getValue();
			if (newValue != oldValue) {
				_rnboObject.setParameterValueNormalized(_index, newValue);
			}
		}

		float getDefaultValue() const override
		{
			return _defaultValue;
		}

		String getParameterID() const override
		{
			return String(_rnboObject.getParameterId(_index));
		}

		String getName (int maximumStringLength) const override
		{
			return _name.substring(0, maximumStringLength);
		}

		String getLabel() const override
		{
			return _unitName;
		}

		float getValueForText (const String& text) const override
		{
			// this is never called
			// does it want the normalized value or not?
			// we probably should convert to normalized since getText() expects to get a normalized value
			// but I guess it doesn't matter if this is never called.
			return text.getFloatValue();
		}

		String getText (float value, int maximumStringLength) const override
		{
			// we want to print the normalized value
			float displayValue = (float)_rnboObject.convertFromNormalizedParameterValue(_index, value);
			return AudioProcessorParameter::getText(displayValue, maximumStringLength);
		}

		const juce::NormalisableRange<float>& getNormalisableRange () const override
		{
			return _normRange;
		}

		bool isAutomatable() const override { return _automatable; }

	protected:
		ParameterIndex			_index;
		CoreObject&				_rnboObject;
		String _unitName;
		String _name;
		float _defaultValue;
		juce::NormalisableRange<float> _normRange;
		bool _automatable = true;
	};

	class EnumParameter : public FloatParameter
	{
		using String = juce::String;
	public:

		EnumParameter (ParameterIndex index, const ParameterInfo& info, CoreObject& rnboObject, int versionHint = 0, bool automatable = true)
		: FloatParameter(index, info, rnboObject, versionHint, automatable)
		{
			for (Index i = 0; i < static_cast<Index>(info.steps); i++) {
				_enumValues.push_back(info.enumValues[i]);
			}
		}

		String getText (float value, int maximumStringLength) const override
		{
			// we want to print the normalized value
			long displayValue = (long)_rnboObject.convertFromNormalizedParameterValue(_index, value);
			String v;
			if (displayValue >= 0 && static_cast<Index>(displayValue) < _enumValues.size()) {
				v = _enumValues[static_cast<Index>(displayValue)];
			}
			return v.substring(0, maximumStringLength);
		}

	protected:
		std::vector<String>	_enumValues;
	};

} // namespace RNBO

#endif  // #ifndef _RNBO_JUCEPLUGINPROCESSOR_H_
