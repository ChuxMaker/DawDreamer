#pragma once
#include "MidiSerialization.h"
#include "PickleVersion.h"
#include "ProcessorBase.h"

#ifdef BUILD_DAWDREAMER_FAUST
#include <faust/compiler/generator/libfaust.h>
#include <faust/compiler/utils/TMutex.h>
#include <faust/dsp/interpreter-dsp.h>
#include <faust/dsp/llvm-dsp.h>
#include <faust/dsp/poly-interpreter-dsp.h>
#include <faust/dsp/poly-llvm-dsp.h>
#include <faust/export.h>
#include <faust/gui/APIUI.h>
#include <faust/gui/MidiUI.h>
#include <faust/gui/SoundUI.h>
#include <faust/midi/rt-midi.h>

#include <cmath>
#include <map>
#include <unordered_map>

#include "FaustArgvBuilder.h"
#include "FaustSignalAPI.h"

/*
A custom implementation of SoundUI. For a requested soundfile primitive in
Faust, we first try to find it in our Python dictionary of buffers. If it's not
found, we resort to using the parent's JuceReader implementation which is still
capable of loading wav files directly from the filesystem.
*/
class MySoundUI : public SoundUI
{
  private:
    std::map<std::string, std::vector<juce::AudioSampleBuffer>>* m_SoundfileMap;
    int m_sampleRate = -1;

  public:
    MySoundUI(std::map<std::string, std::vector<juce::AudioSampleBuffer>>* soundfileMap,
              const std::vector<std::string>& sound_directories, int sample_rate = -1,
              SoundfileReader* reader = nullptr, bool is_double = false)
        : SoundUI(sound_directories, sample_rate, reader, is_double)
    {
        jassert(soundfileMap);
        m_SoundfileMap = soundfileMap;
        m_sampleRate = sample_rate;
    }

    void addSoundfile(const char* label, const char* url, Soundfile** sf_zone)
    {
        // Parse the possible list
        std::string saved_url_real = std::string(label);
        if (fSoundfileMap.find(saved_url_real) != fSoundfileMap.end())
        {
            // Get the soundfile.
            *sf_zone = fSoundfileMap[saved_url_real].get();
            return;
        }

        if (m_SoundfileMap->find(saved_url_real) != m_SoundfileMap->end())
        {
            int total_length = 0;
            int numChannels = 1; // start with at least 1 channel. This may
                                 // increase due to code below.

            auto buffers = m_SoundfileMap->at(saved_url_real);

            for (auto& buffer : buffers)
            {
                total_length += buffer.getNumSamples();
                numChannels = std::max(numChannels, buffer.getNumChannels());
            }

            total_length += (MAX_SOUNDFILE_PARTS - buffers.size()) * BUFFER_SIZE;

            Soundfile* soundfile =
                new Soundfile(numChannels, total_length, MAX_CHAN, (int)buffers.size(), false);

            // Manually fill in the soundfile:
            // The following code is a modification of
            // SoundfileReader::createSoundfile and SoundfileReader::readFile

            int offset = 0;

            int i = 0;
            for (auto& buffer : buffers)
            {
                int numSamples = buffer.getNumSamples();

                soundfile->fLength[i] = numSamples;
                soundfile->fSR[i] = m_sampleRate;
                soundfile->fOffset[i] = offset;

                void* tmpBuffers = alloca(soundfile->fChannels * sizeof(float*));
                soundfile->getBuffersOffsetReal<float>(tmpBuffers, offset);

                for (int chan = 0; chan < buffer.getNumChannels(); chan++)
                {
                    for (int sample = 0; sample < numSamples; sample++)
                    {
                        // todo: don't assume float
                        // todo: use memcpy or similar to be faster
                        static_cast<float**>(soundfile->fBuffers)[chan][offset + sample] =
                            buffer.getSample(chan, sample);
                    }
                }

                offset += soundfile->fLength[i];
                i++;
            }

            // Complete with empty parts
            for (auto i = (int)buffers.size(); i < MAX_SOUNDFILE_PARTS; i++)
            {
                soundfile->emptyFile(i, offset);
            }

            // Share the same buffers for all other channels so that we have
            // max_chan channels available
            soundfile->shareBuffers(numChannels, MAX_CHAN);
            fSoundfileMap[saved_url_real] = std::shared_ptr<Soundfile>(soundfile);

            // Get the soundfile pointer
            *sf_zone = fSoundfileMap[saved_url_real].get();
            return;
        }

        // The requested sound url wasn't in our python dictionary, so use the
        // inherited method to load it from the filesystem.
        SoundUI::addSoundfile(label, url, sf_zone);
    }
};

template <typename Ch, typename Traits = std::char_traits<Ch>, typename Sequence = std::vector<Ch>>
struct basic_seqbuf : std::basic_streambuf<Ch, Traits>
{
    typedef std::basic_streambuf<Ch, Traits> base_type;
    typedef typename base_type::int_type int_type;
    typedef typename base_type::traits_type traits_type;

    virtual int_type overflow(int_type ch)
    {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
            return traits_type::eof();
        c.push_back(traits_type::to_char_type(ch));
        return ch;
    }

    Sequence const& get_sequence() const { return c; }

  protected:
    Sequence c;
};

// convenient typedefs
typedef basic_seqbuf<char> seqbuf;

class FaustProcessor : public ProcessorBase
{
  public:
    FaustProcessor(std::string newUniqueName, double sampleRate, int samplesPerBlock);
    ~FaustProcessor();

    bool canApplyBusesLayout(const juce::AudioProcessor::BusesLayout& layout) override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    int getTotalNumOutputChannels() override
    {
        if (!m_compileState)
        {
            this->compile();
        }
        return ProcessorBase::getTotalNumOutputChannels();
    }

    int getTotalNumInputChannels() override
    {
        if (!m_compileState)
        {
            this->compile();
        }
        return ProcessorBase::getTotalNumInputChannels();
    }

    void processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer& midiBuffer) override;

    bool acceptsMidi() const override { return false; } // todo: faust should be able to play MIDI.
    bool producesMidi() const override { return false; }

    void reset() override;

    void createParameterLayout();

    const juce::String getName() const override { return "FaustProcessor"; }

    void automateParameters(AudioPlayHead::PositionInfo& posInfo, int numSamples) override;
    bool setAutomation(std::string& parameterName, nb::ndarray<float> input,
                       std::uint32_t ppqn) override;

    // faust stuff
    void clear();
    bool compile();
    bool compileFromBitcode(const std::string& bitcode);
    bool initFromFactory();
    bool setDSPString(const std::string& code);
    bool setDSPFile(const std::string& path);
    bool setParamWithIndex(const int index, float p);
    float getParamWithIndex(const int index);
    float getParamWithPath(const std::string& n);
    std::string code();
    bool isCompiled() { return bool(m_compileState); };

    nb::list getPluginParametersDescription();

    void setNumVoices(int numVoices);
    int getNumVoices();

    void setGroupVoices(bool groupVoices);
    int getGroupVoices();

    void setDynamicVoices(bool dynamicVoices);
    int getDynamicVoices();

    void setAutoImport(const std::string& s) { m_autoImport = s; }
    std::string getAutoImport() { return m_autoImport; }

    bool loadMidi(const std::string& path, bool clearPrevious, bool isBeats, bool allEvents);

    void clearMidi();

    int getNumMidiEvents();

    bool addMidiNote(const uint8 midiNote, const uint8 midiVelocity, const double noteStart,
                     const double noteLength, bool isBeats);

    void setSoundfiles(nb::dict);

    double getReleaseLength();

    void setReleaseLength(double sec);

    // --- Per-note expression (Tier-A, aifi WS16) ---------------------------------
    // A per-voice parameter set from each note's velocity:
    //   base = lo + (hi - lo) * (velocity/127)^curve
    // written directly to the dsp_voice that keyOn() returns (a MapUI), so it is
    // genuinely per-note and bypasses the grouped UI -- the offline, deterministic,
    // fork-only path the stock wheel cannot do (it supersedes realize's running-gain
    // "win A" hack). With settle==1 or tau<=0 the value is STATIC (set once at
    // note-on). With tau>0 and settle!=1 it VARIES over the note's life (the envelope
    // / "deepen" step), modulated host-side per (control-rate) sample in processBlock:
    //   value(age) = base * (settle + (1 - settle) * exp(-age/tau))
    // settle<1 is a brightness DECAY (bright onset, mellows); settle>1 is a SWELL.
    // Default-off ⇒ byte-identical to stock.
    struct NoteExprState
    {
        double base;
        long long startSample;
    };
    static constexpr int kNoteExprStride = 64;   // control-rate update period (samples)

    void setNoteExpression(const std::string& param, double lo, double hi, double curve,
                           double settle, double tau)
    {
        m_noteExprParam = param;
        m_noteExprLo = lo;
        m_noteExprHi = hi;
        m_noteExprCurve = (curve > 0.0) ? curve : 1.0;
        m_noteExprSettle = settle;
        m_noteExprTau = tau;
        m_noteExprEnabled = !param.empty();
        m_noteExprDynamic = m_noteExprEnabled && tau > 0.0 && settle != 1.0;
        m_activeExpr.clear();
    }

    void clearNoteExpression()
    {
        m_noteExprEnabled = false;
        m_noteExprDynamic = false;
        m_activeExpr.clear();
    }

    // base (onset / static) expression value for a note's velocity.
    double noteExprBase(int velocity) const
    {
        double norm = double(velocity) / 127.0;
        if (norm < 0.0) norm = 0.0;
        if (norm > 1.0) norm = 1.0;
        double shaped = (m_noteExprCurve == 1.0) ? norm : std::pow(norm, m_noteExprCurve);
        return m_noteExprLo + (m_noteExprHi - m_noteExprLo) * shaped;
    }

    // Called at each keyOn with the voice keyOn returned + the absolute sample index.
    // Sets the onset value immediately (note start, amplitude ~0 -> no click) and, when
    // the envelope is dynamic, registers the voice so updateNoteExpression() can modulate
    // it over the note. Keyed by voice pointer (bounded by num_voices; a reused/stolen
    // voice's entry is overwritten by its next note).
    void applyNoteExpression(MapUI* voice, int velocity, long long sample)
    {
        if (!m_noteExprEnabled || voice == nullptr)
            return;
        double base = noteExprBase(velocity);
        voice->setParamValue(m_noteExprParam, FAUSTFLOAT(base));
        if (m_noteExprDynamic)
            m_activeExpr[voice] = NoteExprState{base, sample};
    }

    // Control-rate update of every active voice's expression param toward its settle
    // value. Called every kNoteExprStride samples from processBlock (the envelope is slow
    // vs audio rate, so control-rate stepping is inaudible and keeps render fast).
    void updateNoteExpression(long long sample)
    {
        if (!m_noteExprDynamic || m_activeExpr.empty())
            return;
        for (auto& kv : m_activeExpr)
        {
            double age = double(sample - kv.second.startSample) / mySampleRate;
            if (age < 0.0) age = 0.0;
            double env = m_noteExprSettle +
                         (1.0 - m_noteExprSettle) * std::exp(-age / m_noteExprTau);
            kv.first->setParamValue(m_noteExprParam, FAUSTFLOAT(kv.second.base * env));
        }
    }

    nb::dict getPickleState()
    {
        nb::dict state;
        state["pickle_version"] = DawDreamerPickle::getVersion();
        state["unique_name"] = getUniqueName();
        state["sample_rate"] = mySampleRate;
        state["faust_code"] = m_code;
        state["auto_import"] = m_autoImport;

        // Convert library paths vector to list
        nb::list lib_paths;
        for (const auto& path : m_faustLibrariesPaths)
            lib_paths.append(path);
        state["library_paths"] = lib_paths;

        // Convert asset paths vector to list
        nb::list asset_paths;
        for (const auto& path : m_faustAssetsPaths)
            asset_paths.append(path);
        state["asset_paths"] = asset_paths;

        // Convert compile flags vector to list
        nb::list compile_flags;
        for (const auto& flag : m_compileFlags)
            compile_flags.append(flag);
        state["compile_flags"] = compile_flags;

        state["num_voices"] = m_nvoices;
        state["group_voices"] = m_groupVoices;
        state["dynamic_voices"] = m_dynamicVoices;
        state["llvm_opt_level"] = m_llvmOptLevel;
        state["release_length"] = m_releaseLengthSec;

        // Per-note expression (pickle v2)
        state["note_expr_enabled"] = m_noteExprEnabled;
        state["note_expr_param"] = m_noteExprParam;
        state["note_expr_lo"] = m_noteExprLo;
        state["note_expr_hi"] = m_noteExprHi;
        state["note_expr_curve"] = m_noteExprCurve;
        state["note_expr_settle"] = m_noteExprSettle;
        state["note_expr_tau"] = m_noteExprTau;

        // Soundfiles
        nb::dict soundfiles;
        for (const auto& [name, buffers] : m_SoundfileMap)
        {
            nb::list buffer_list;
            for (auto& buffer : buffers)
            {
                // Need to make a non-const copy for bufferToPyArray
                juce::AudioSampleBuffer buf_copy(buffer);
                buffer_list.append(bufferToPyArray(buf_copy));
            }
            soundfiles[name.c_str()] = buffer_list;
        }
        state["soundfiles"] = soundfiles;

        // Parameters
        nb::dict params;
        for (auto* parameter : getParameters())
        {
            std::string name = parameter->getName(DAW_PARAMETER_MAX_NAME_LENGTH).toStdString();
            if (!name.empty())
            {
                // Cast to AutomateParameterFloat to access automation value
                auto* autoParam = (AutomateParameterFloat*)parameter;
                auto automation = autoParam->getAutomation();
                if (!automation.empty())
                {
                    params[name.c_str()] = automation[0];
                }
                else
                {
                    params[name.c_str()] = parameter->getValue();
                }
            }
        }
        state["parameters"] = params;

        // Serialize MIDI buffers using shared helper
        state["midi_qn"] = MidiSerialization::serializeMidiBuffer(myMidiBufferQN);
        state["midi_sec"] = MidiSerialization::serializeMidiBuffer(myMidiBufferSec);

        // Serialize compiled LLVM bitcode to avoid recompilation on unpickle.
        // Only supported for mono (non-polyphonic) factories.
        // Polyphonic factories fall back to recompilation from source.
        if (m_factory)
        {
            state["bitcode"] = writeDSPFactoryToBitcode(m_factory);
        }

        return state;
    }

    void setPickleState(nb::dict state)
    {
        // Check pickle version
        if (state.contains("pickle_version"))
        {
            int version = nb::cast<int>(state["pickle_version"]);
            if (!DawDreamerPickle::isCompatibleVersion(version))
            {
                throw std::runtime_error(DawDreamerPickle::getVersionErrorMessage(version));
            }
        }

        std::string name = nb::cast<std::string>(state["unique_name"]);
        double sample_rate = nb::cast<double>(state["sample_rate"]);
        int buffer_size = 512; // Default, will be updated by prepareToPlay

        // Use placement new to construct the object in-place
        new (this) FaustProcessor(name, sample_rate, buffer_size);

        // Set Faust code and configuration
        m_code = nb::cast<std::string>(state["faust_code"]);
        m_autoImport = nb::cast<std::string>(state["auto_import"]);

        // Library paths
        if (state.contains("library_paths"))
        {
            nb::list lib_paths = nb::cast<nb::list>(state["library_paths"]);
            m_faustLibrariesPaths.clear();
            for (nb::handle path : lib_paths)
                m_faustLibrariesPaths.push_back(nb::cast<std::string>(path));
        }

        // Asset paths
        if (state.contains("asset_paths"))
        {
            nb::list asset_paths = nb::cast<nb::list>(state["asset_paths"]);
            m_faustAssetsPaths.clear();
            for (nb::handle path : asset_paths)
                m_faustAssetsPaths.push_back(nb::cast<std::string>(path));
        }

        // Compile flags
        if (state.contains("compile_flags"))
        {
            nb::list compile_flags = nb::cast<nb::list>(state["compile_flags"]);
            m_compileFlags.clear();
            for (nb::handle flag : compile_flags)
                m_compileFlags.push_back(nb::cast<std::string>(flag));
        }

        m_nvoices = nb::cast<int>(state["num_voices"]);
        m_groupVoices = nb::cast<bool>(state["group_voices"]);
        m_dynamicVoices = nb::cast<bool>(state["dynamic_voices"]);
        m_llvmOptLevel = nb::cast<int>(state["llvm_opt_level"]);
        m_releaseLengthSec = nb::cast<double>(state["release_length"]);

        // Per-note expression (pickle v2; absent in v1 ⇒ stays disabled)
        if (state.contains("note_expr_enabled"))
            m_noteExprEnabled = nb::cast<bool>(state["note_expr_enabled"]);
        if (state.contains("note_expr_param"))
            m_noteExprParam = nb::cast<std::string>(state["note_expr_param"]);
        if (state.contains("note_expr_lo"))
            m_noteExprLo = nb::cast<double>(state["note_expr_lo"]);
        if (state.contains("note_expr_hi"))
            m_noteExprHi = nb::cast<double>(state["note_expr_hi"]);
        if (state.contains("note_expr_curve"))
            m_noteExprCurve = nb::cast<double>(state["note_expr_curve"]);
        if (state.contains("note_expr_settle"))
            m_noteExprSettle = nb::cast<double>(state["note_expr_settle"]);
        if (state.contains("note_expr_tau"))
            m_noteExprTau = nb::cast<double>(state["note_expr_tau"]);
        m_noteExprDynamic =
            m_noteExprEnabled && m_noteExprTau > 0.0 && m_noteExprSettle != 1.0;

        // Restore from compiled LLVM bitcode if available (fast path),
        // otherwise fall back to full recompilation from source (slow path).
        if (state.contains("bitcode"))
        {
            std::string bitcode = nb::cast<std::string>(state["bitcode"]);
            std::string error_msg;
            m_factory = readDSPFactoryFromBitcode(bitcode, getTarget(), error_msg, m_llvmOptLevel);
            if (!m_factory)
            {
                throw std::runtime_error(
                    "FaustProcessor::setPickleState(): Failed to restore factory "
                    "from bitcode: " +
                    error_msg);
            }
            initFromFactory();
        }
        else if (!m_code.empty())
        {
            compile();
        }

        // Restore soundfiles
        if (state.contains("soundfiles"))
        {
            setSoundfiles(nb::cast<nb::dict>(state["soundfiles"]));
        }

        // Restore parameter values
        if (state.contains("parameters"))
        {
            nb::dict params = nb::cast<nb::dict>(state["parameters"]);
            for (auto item : params)
            {
                std::string param_name = nb::cast<std::string>(item.first);
                float param_value = nb::cast<float>(item.second);
                setAutomationVal(param_name.c_str(), param_value);
            }
        }

        // Restore MIDI buffers using shared helper
        if (state.contains("midi_qn"))
        {
            nb::bytes midi_qn_data = nb::cast<nb::bytes>(state["midi_qn"]);
            MidiSerialization::deserializeMidiBuffer(myMidiBufferQN, midi_qn_data);
        }

        if (state.contains("midi_sec"))
        {
            nb::bytes midi_sec_data = nb::cast<nb::bytes>(state["midi_sec"]);
            MidiSerialization::deserializeMidiBuffer(myMidiBufferSec, midi_sec_data);
        }
    }

    void setFaustLibrariesPath(std::string faustLibrariesPath)
    {
        m_faustLibrariesPaths.clear();
        m_faustLibrariesPaths.push_back(faustLibrariesPath);
    }

    void setFaustLibrariesPaths(std::vector<std::string> faustLibrariesPaths)
    {
        m_faustLibrariesPaths.clear();
        m_faustLibrariesPaths = faustLibrariesPaths;
    }

    std::string getFaustLibrariesPath()
    {
        if (!m_faustLibrariesPaths.empty())
        {
            return m_faustLibrariesPaths.at(0);
        }
        return "";
    }

    std::vector<std::string> getFaustLibrariesPaths() { return m_faustLibrariesPaths; }

    void setFaustAssetsPath(std::string faustAssetsPath)
    {
        m_faustAssetsPaths.clear();
        m_faustAssetsPaths.push_back(faustAssetsPath);
    }

    void setFaustAssetsPaths(std::vector<std::string> faustAssetsPath)
    {
        m_faustAssetsPaths.clear();
        m_faustAssetsPaths = faustAssetsPath;
    }

    std::string getFaustAssetsPath()
    {
        if (!m_faustAssetsPaths.empty())
        {
            return m_faustAssetsPaths.at(0);
        }
        return "";
    }

    std::vector<std::string> getFaustAssetsPaths() { return m_faustAssetsPaths; }

    void setCompileFlags(std::vector<std::string> compileFlags) { m_compileFlags = compileFlags; }

    std::vector<std::string> getCompileFlags() { return m_compileFlags; }

    void setLLVMOpt(int optLevel)
    {
        if (m_llvmOptLevel != optLevel)
        {
            m_compileState = kNotCompiled;
        }
        m_llvmOptLevel = optLevel;
    }

    int getLLVMOpt() { return m_llvmOptLevel; }

    std::map<std::string, std::vector<juce::AudioSampleBuffer>> m_SoundfileMap;

    void saveMIDI(std::string& savePath);

  private:
    double mySampleRate;

    enum CompileState
    {
        kNotCompiled,
        kMono,
        kPoly,
        kSignalMono,
        kSignalPoly
    };

    CompileState m_compileState;

  protected:
    llvm_dsp_factory* m_factory = nullptr;
    llvm_dsp_poly_factory* m_poly_factory = nullptr;

    dsp* m_dsp = nullptr;
    dsp_poly* m_dsp_poly = nullptr;

    APIUI* m_ui = nullptr;
    MySoundUI* m_soundUI = nullptr;

    rt_midi m_midi_handler;

    int m_numInputChannels = 0;
    int m_numOutputChannels = 0;

    double m_releaseLengthSec = 0.5;

    std::string m_autoImport;
    std::string m_code;
    std::vector<std::string> m_faustLibrariesPaths;
    std::vector<std::string> m_faustAssetsPaths;
    std::vector<std::string> m_compileFlags;

    int m_nvoices = 0;
    bool m_dynamicVoices = true;
    bool m_groupVoices = true;
    int m_llvmOptLevel = -1;

    // Per-note expression (Tier-A, WS16). Default-off ⇒ stock-identical render path.
    bool m_noteExprEnabled = false;
    bool m_noteExprDynamic = false;     // tau>0 && settle!=1 -> modulate over each note
    std::string m_noteExprParam;
    double m_noteExprLo = 0.0;
    double m_noteExprHi = 1.0;
    double m_noteExprCurve = 1.0;
    double m_noteExprSettle = 1.0;      // asymptote multiplier (<1 decay, >1 swell)
    double m_noteExprTau = 0.0;         // envelope time constant (seconds); <=0 = static
    std::unordered_map<MapUI*, NoteExprState> m_activeExpr;   // active voices (<= num_voices)

    MidiBuffer myMidiBufferQN;
    MidiBuffer myMidiBufferSec;

    MidiMessageSequence myRecordedMidiSequence; // for fetching by user later.

    MidiMessage myMidiMessageQN;
    MidiMessage myMidiMessageSec;

    int myMidiMessagePositionQN = -1;
    int myMidiMessagePositionSec = -1;

    MidiBuffer::Iterator* myMidiIteratorQN = nullptr;
    MidiBuffer::Iterator* myMidiIteratorSec = nullptr;

    bool myIsMessageBetweenQN = false;
    bool myIsMessageBetweenSec = false;

    bool myMidiEventsDoRemainQN = false;
    bool myMidiEventsDoRemainSec = false;

    juce::AudioSampleBuffer oneSampleInBuffer;
    juce::AudioSampleBuffer oneSampleOutBuffer;

    std::map<int, int> m_map_juceIndex_to_faustIndex;
    std::map<int, std::string> m_map_juceIndex_to_parAddress;

    TMutex guiUpdateMutex;

    std::string getTarget();

    // public libfaust API
  public:
    bool compileSignals(std::vector<SigWrapper>& wrappers);
    bool compileSignals(std::vector<SigWrapper>& wrappers, const std::vector<std::string>& in_argv);

    bool compileBox(BoxWrapper& box);
    bool compileBox(BoxWrapper& box, const std::vector<std::string>& in_argv);
};

inline void create_bindings_for_faust_processor(nb::module_& m)
{
    using arg = nb::arg;
    using kw_only = nb::kw_only;

    auto returnPolicy = nb::rv_policy::take_ownership;

    // todo: for consistency, lookup these descriptions from source.cpp
    auto add_midi_description =
        "Add a single MIDI note whose note and velocity are integers between 0 "
        "and 127. By default, when `beats` is False, the start time and duration "
        "are measured in seconds, otherwise beats.";
    auto load_midi_description =
        "Load MIDI from a file. If `all_events` is True, then all events (not "
        "just Note On/Off) will be loaded. By default, when `beats` is False, "
        "notes will be converted to absolute times and will not be affected by "
        "the Render Engine's BPM. By default, `clear_previous` is True.";
    auto save_midi_description =
        "After rendering, you can save the MIDI to a file using absolute times "
        "(SMPTE format).";

    nb::class_<FaustProcessor, ProcessorBase> faustProcessor(m, "FaustProcessor");

    faustProcessor
        .def("set_dsp", &FaustProcessor::setDSPFile, arg("filepath"),
             "Set the FAUST box process with a file.")
        .def("set_dsp_string", &FaustProcessor::setDSPString, arg("faust_code"),
             "Set the FAUST box process with a string containing FAUST code.")
        .def("compile", &FaustProcessor::compile,
             "Compile the FAUST object. You must have already set a dsp file "
             "path or dsp string.")
        .def("compile_from_bitcode", &FaustProcessor::compileFromBitcode, arg("bitcode"),
             "Restore a compiled FAUST object from LLVM bitcode, avoiding "
             "recompilation from source. Bitcode can be obtained from the "
             "processor's pickle state dict.")
        .def_prop_rw("auto_import", &FaustProcessor::getAutoImport, &FaustProcessor::setAutoImport,
                     "The auto import string. Default is `import(\"stdfaust.lib\");`")
        .def("get_parameters_description", &FaustProcessor::getPluginParametersDescription,
             "Get a list of dictionaries describing the parameters of the most "
             "recently compiled FAUST code.")
        .def("get_parameter", &FaustProcessor::getParamWithIndex, arg("param_index"))
        .def("get_parameter", &FaustProcessor::getParamWithPath, arg("parameter_path"))
        .def("set_parameter", &FaustProcessor::setParamWithIndex, arg("parameter_index"),
             arg("value"))
        .def("set_parameter", &FaustProcessor::setAutomationVal, arg("parameter_path"),
             arg("value"))
        .def_prop_ro("compiled", &FaustProcessor::isCompiled,
                     "Did the most recent DSP code compile?")
        .def_prop_ro("code", &FaustProcessor::code,
                     "Get the most recently compiled Faust DSP code.")
        .def_prop_rw("num_voices", &FaustProcessor::getNumVoices, &FaustProcessor::setNumVoices,
                     "The number of voices for polyphony. Set to zero to "
                     "disable polyphony. One or more enables polyphony.")
        .def_prop_rw("group_voices", &FaustProcessor::getGroupVoices,
                     &FaustProcessor::setGroupVoices,
                     "If grouped, all polyphonic voices will share the same parameters. "
                     "This parameter only matters if polyphony is enabled.")
        .def_prop_rw("dynamic_voices", &FaustProcessor::getDynamicVoices,
                     &FaustProcessor::setDynamicVoices,
                     "If enabled (default), voices are dynamically enabled and "
                     "disabled to save computation. This parameter only matters "
                     "if polyphony is enabled.")
        .def_prop_rw("release_length", &FaustProcessor::getReleaseLength,
                     &FaustProcessor::setReleaseLength,
                     "If using polyphony, specifying the release length accurately can "
                     "help avoid warnings about voices being stolen.")
        .def_prop_rw("faust_libraries_path", &FaustProcessor::getFaustLibrariesPath,
                     &FaustProcessor::setFaustLibrariesPath,
                     "Absolute path to directory containing your custom "
                     "\".lib\" files containing Faust code.")
        .def_prop_rw("faust_libraries_paths", &FaustProcessor::getFaustLibrariesPaths,
                     &FaustProcessor::setFaustLibrariesPaths,
                     "List of absolute paths to directories containing your custom "
                     "\".lib\" files containing Faust code.")
        .def_prop_rw("faust_assets_path", &FaustProcessor::getFaustAssetsPath,
                     &FaustProcessor::setFaustAssetsPath,
                     "Absolute path to directory containing audio files to be "
                     "used by Faust.")
        .def_prop_rw("faust_assets_paths", &FaustProcessor::getFaustAssetsPaths,
                     &FaustProcessor::setFaustAssetsPaths,
                     "List of absolute paths to directories containing audio files to be "
                     "used by Faust.")
        .def_prop_rw("compile_flags", &FaustProcessor::getCompileFlags,
                     &FaustProcessor::setCompileFlags, "List of compilation flags.")
        .def_prop_rw("opt_level", &FaustProcessor::getLLVMOpt, &FaustProcessor::setLLVMOpt,
                     "LLVM IR to IR optimization level (from -1 to 4, -1 means 'maximum "
                     "possible value' * since the maximum value may change with new LLVM "
                     "versions)")
        .def_prop_ro("n_midi_events", &FaustProcessor::getNumMidiEvents,
                     "The number of MIDI events stored in the buffer. \
		Note that note-ons and note-offs are counted separately.")
        .def("load_midi", &FaustProcessor::loadMidi, arg("filepath"), kw_only(),
             arg("clear_previous") = true, arg("beats") = false, arg("all_events") = true,
             load_midi_description)
        .def("clear_midi", &FaustProcessor::clearMidi, "Remove all MIDI notes.")
        .def("add_midi_note", &FaustProcessor::addMidiNote, arg("note"), arg("velocity"),
             arg("start_time"), arg("duration"), kw_only(), arg("beats") = false,
             add_midi_description)
        .def("set_note_expression", &FaustProcessor::setNoteExpression, arg("parameter"),
             kw_only(), arg("lo") = 0.0, arg("hi") = 1.0, arg("curve") = 1.0,
             arg("settle") = 1.0, arg("tau") = 0.0,
             "Per-note expression (Tier-A). At every note-on, set the named per-voice "
             "parameter (matched by path, shortname, or label) to a base value "
             "lo + (hi - lo) * (velocity/127)**curve, written directly to the voice that "
             "starts the note -- deterministic, genuinely per-voice expression (e.g. "
             "louder notes brighter) an offline render otherwise cannot do. With settle!=1 "
             "and tau>0 the value also varies over each note's life: "
             "base * (settle + (1 - settle) * exp(-age_seconds/tau)) -- settle<1 decays "
             "(bright onset, mellows), settle>1 swells. settle==1 or tau<=0 is static. "
             "Pass an empty parameter name, or call clear_note_expression(), to disable.")
        .def("clear_note_expression", &FaustProcessor::clearNoteExpression,
             "Disable per-note expression previously set by set_note_expression().")
        .def("save_midi", &FaustProcessor::saveMIDI, arg("filepath"), save_midi_description)
        .def("set_soundfiles", &FaustProcessor::setSoundfiles, arg("soundfile_dict"),
             "Set the audio data that the FaustProcessor can use with the "
             "`soundfile` primitive.")

        .def("compile_signals",
             nb::overload_cast<std::vector<SigWrapper>&>(&FaustProcessor::compileSignals),
             arg("signals"), returnPolicy)
        .def("compile_signals",
             nb::overload_cast<std::vector<SigWrapper>&, const std::vector<std::string>&>(
                 &FaustProcessor::compileSignals),
             arg("signals"), arg("argv"), returnPolicy)
        .def("compile_box", nb::overload_cast<BoxWrapper&>(&FaustProcessor::compileBox), arg("box"),
             returnPolicy)
        .def("compile_box",
             nb::overload_cast<BoxWrapper&, const std::vector<std::string>&>(
                 &FaustProcessor::compileBox),
             arg("box"), arg("argv"), returnPolicy)

        .def("__getstate__", &FaustProcessor::getPickleState)
        .def("__setstate__", &FaustProcessor::setPickleState)

        .doc() = "A Faust Processor can compile and execute FAUST code. See "
                 "https://faust.grame.fr for more information.";
}

#endif
