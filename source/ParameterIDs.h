#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Central definition of every plugin parameter.

    Keeping the ids, ranges and defaults in one place means the processor, the
    editor and any future tests all agree on exactly what exists.
*/
namespace ParamID
{
    inline constexpr auto input   = "input";
    inline constexpr auto polish  = "polish";
    inline constexpr auto low     = "low";
    inline constexpr auto high    = "high";
    inline constexpr auto tilt    = "tilt";
    inline constexpr auto drive   = "drive";
    inline constexpr auto glue    = "glue";
    inline constexpr auto width   = "width";
    inline constexpr auto ceiling = "ceiling";
    inline constexpr auto output  = "output";
    inline constexpr auto bypass  = "bypass";
}

namespace AudioPolishParams
{
    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using APF   = juce::AudioParameterFloat;
        using APB   = juce::AudioParameterBool;
        using Range = juce::NormalisableRange<float>;
        using ID    = juce::ParameterID;

        // Bump this whenever the set of parameters changes so hosts re-scan.
        constexpr int version = 1;

        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        auto db = [] (float lo, float hi) { return Range { lo, hi, 0.01f }; };

        params.push_back (std::make_unique<APF> (ID { ParamID::input, version }, "Input",
                                                 db (-24.0f, 24.0f), 0.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::polish, version }, "Polish",
                                                 Range { 0.0f, 100.0f, 0.1f }, 25.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::low, version }, "Low",
                                                 db (-12.0f, 12.0f), 0.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::high, version }, "High",
                                                 db (-12.0f, 12.0f), 0.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::tilt, version }, "Tilt",
                                                 db (-6.0f, 6.0f), 0.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::drive, version }, "Drive",
                                                 Range { 0.0f, 100.0f, 0.1f }, 20.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::glue, version }, "Glue",
                                                 Range { 0.0f, 100.0f, 0.1f }, 25.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::width, version }, "Width",
                                                 Range { 0.0f, 200.0f, 0.1f }, 100.0f));

        params.push_back (std::make_unique<APF> (ID { ParamID::ceiling, version }, "Ceiling",
                                                 db (-12.0f, 0.0f), -0.3f));

        params.push_back (std::make_unique<APF> (ID { ParamID::output, version }, "Output",
                                                 db (-24.0f, 24.0f), 0.0f));

        params.push_back (std::make_unique<APB> (ID { ParamID::bypass, version }, "Bypass", false));

        return { params.begin(), params.end() };
    }
}
