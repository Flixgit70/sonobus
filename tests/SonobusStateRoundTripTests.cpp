#include "SonobusPluginProcessor.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const juce::String& message)
{
    if (!condition)
        throw std::runtime_error(message.toStdString());
}

void setParameter(juce::AudioProcessorValueTreeState& state, const juce::String& id, float normalizedValue)
{
    auto* parameter = state.getParameter(id);
    require(parameter != nullptr, "Missing parameter: " + id);
    parameter->setValueNotifyingHost(normalizedValue);
}

void compareParameters(juce::AudioProcessor& expected, juce::AudioProcessor& actual)
{
    const auto& expectedParameters = expected.getParameters();
    const auto& actualParameters = actual.getParameters();
    require(expectedParameters.size() == actualParameters.size(), "VST parameter count changed during state restore");

    for (int i = 0; i < expectedParameters.size(); ++i)
    {
        const auto expectedValue = expectedParameters.getUnchecked(i)->getValue();
        const auto actualValue = actualParameters.getUnchecked(i)->getValue();
        if (std::abs(expectedValue - actualValue) > 0.0001f)
        {
            throw std::runtime_error("VST parameter '" + expectedParameters.getUnchecked(i)->getName(128).toStdString() +
                                     "' at index " + std::to_string(i) + " did not round-trip (" +
                                     std::to_string(expectedValue) + " -> " + std::to_string(actualValue) + ")");
        }
    }
}

void comparePatchbay(SonobusAudioProcessor& expected, SonobusAudioProcessor& actual)
{
    require(actual.getPatchbaySendRoute(2, 5), "Send crosspoint did not round-trip");
    require(!actual.getPatchbaySendRoute(2, 6), "Disconnected send crosspoint became active");
    require(std::abs(actual.getPatchbaySendInputGain(2) - 0.42f) < 0.0001f, "Send input gain did not round-trip");
    require(std::abs(actual.getPatchbaySendOutputGain(5) - 0.71f) < 0.0001f, "Send output gain did not round-trip");
    require(std::abs(actual.getPatchbaySendRouteGain(2, 5) - 0.83f) < 0.0001f, "Send crosspoint gain did not round-trip");

    require(actual.getPatchbayReceiveRoute(3, 4, 7), "Receive crosspoint did not round-trip");
    require(std::abs(actual.getPatchbayReceiveInputGain(3, 4) - 0.55f) < 0.0001f, "Receive input gain did not round-trip");
    require(std::abs(actual.getPatchbayReceiveOutputGain(7) - 0.66f) < 0.0001f, "Receive output gain did not round-trip");
    require(std::abs(actual.getPatchbayReceiveRouteGain(3, 4, 7) - 0.77f) < 0.0001f, "Receive crosspoint gain did not round-trip");

    require(actual.getPatchbayInputLabel(2) == "Vocal Mic", "Input label did not round-trip");
    require(actual.getPatchbayOutputLabel(7) == "Control Room L", "Output label did not round-trip");
    require(actual.getPatchbayShowUserLabels() == expected.getPatchbayShowUserLabels(), "Patchbay label mode did not round-trip");
}

void setPatchbayValues(SonobusAudioProcessor& processor)
{
    processor.setPatchbaySendRoute(2, 5, true);
    processor.setPatchbaySendInputGain(2, 0.42f);
    processor.setPatchbaySendOutputGain(5, 0.71f);
    processor.setPatchbaySendRouteGain(2, 5, 0.83f);

    processor.setPatchbayReceiveRoute(3, 4, 7, true);
    processor.setPatchbayReceiveInputGain(3, 4, 0.55f);
    processor.setPatchbayReceiveOutputGain(7, 0.66f);
    processor.setPatchbayReceiveRouteGain(3, 4, 7, 0.77f);

    processor.setPatchbayInputLabel(2, "Vocal Mic");
    processor.setPatchbayOutputLabel(7, "Control Room L");
    processor.setPatchbayShowUserLabels(false);
}

bool expectedSendRoute(int input, int stream)
{
    return (input * 3 + stream * 5) % 7 < 2;
}

bool expectedReceiveRoute(int peer, int stream, int output)
{
    return (peer * 13 + stream * 3 + output * 5) % 23 == 0;
}

void setDensePatchbayMatrix(SonobusAudioProcessor& processor)
{
    processor.clearPatchbayRoutes();
    for (int input = 0; input < MAX_CHANNELS; ++input)
        for (int stream = 0; stream < MAX_CHANNELS; ++stream)
            if (expectedSendRoute(input, stream))
                processor.setPatchbaySendRoute(input, stream, true);

    for (int peer = 0; peer < MAX_PEERS; ++peer)
        for (int stream = 0; stream < MAX_CHANNELS; ++stream)
            for (int output = 0; output < MAX_CHANNELS; ++output)
                if (expectedReceiveRoute(peer, stream, output))
                    processor.setPatchbayReceiveRoute(peer, stream, output, true);
}

void compareDensePatchbayMatrix(SonobusAudioProcessor& actual)
{
    for (int input = 0; input < MAX_CHANNELS; ++input)
        for (int stream = 0; stream < MAX_CHANNELS; ++stream)
            require(actual.getPatchbaySendRoute(input, stream) == expectedSendRoute(input, stream),
                    "Send crosspoint matrix did not fully survive a repeated save/load");

    for (int peer = 0; peer < MAX_PEERS; ++peer)
        for (int stream = 0; stream < MAX_CHANNELS; ++stream)
            for (int output = 0; output < MAX_CHANNELS; ++output)
                require(actual.getPatchbayReceiveRoute(peer, stream, output) == expectedReceiveRoute(peer, stream, output),
                        "Receive crosspoint matrix did not fully survive a repeated save/load");
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
        SonobusAudioProcessor source;
        SonobusAudioProcessor restored;

        auto& sourceState = source.getValueTreeState();
        auto& restoredState = restored.getValueTreeState();
        const auto& parameters = source.getParameters();
        require(!parameters.isEmpty(), "Processor exposed no VST parameters");

        for (int i = 0; i < parameters.size(); ++i)
        {
            const auto* parameter = parameters.getUnchecked(i);
            const float value = parameter->getName(128) == "Reconnect Last" ? 0.0f
                              : (i % 2 == 0 ? 0.0f : 1.0f);
            source.getParameters().getUnchecked(i)->setValueNotifyingHost(value);
        }

        // Exercise bool and choice parameters at their enabled/alternate values too.
        setParameter(sourceState, SonobusAudioProcessor::paramMetEnabled, 1.0f);
        setParameter(sourceState, SonobusAudioProcessor::paramMainMonitorSolo, 1.0f);
        setParameter(sourceState, SonobusAudioProcessor::paramSendChannels, 0.67f);
        setParameter(sourceState, SonobusAudioProcessor::paramPatchbaySendChannelCount, 0.5f);
        setPatchbayValues(source);

        juce::MemoryBlock hostState;
        source.getStateInformation(hostState);
        restored.setStateInformation(hostState.getData(), static_cast<int>(hostState.getSize()));
        compareParameters(source, restored);
        comparePatchbay(source, restored);

        // Preset files in this product use the XML state format.
        juce::MemoryBlock presetState;
        source.getStateInformationWithOptions(presetState, false, true, true);
        setPatchbayValues(restored);
        restored.setStateInformationWithOptions(presetState.getData(), static_cast<int>(presetState.getSize()), false, true, true);
        compareParameters(source, restored);
        comparePatchbay(source, restored);

        // Saving again after a host restore must replace the embedded patchbay
        // snapshot instead of leaving the older PatchbayState node in place.
        setDensePatchbayMatrix(restored);
        SonobusAudioProcessor hostRoundTrip;
        juce::MemoryBlock repeatedHostState;
        restored.getStateInformation(repeatedHostState);
        hostRoundTrip.setStateInformation(repeatedHostState.getData(), static_cast<int>(repeatedHostState.getSize()));
        compareDensePatchbayMatrix(hostRoundTrip);

        // Exercise the same repeated-save path through the XML preset format.
        SonobusAudioProcessor presetRoundTrip;
        juce::MemoryBlock repeatedPresetState;
        restored.getStateInformationWithOptions(repeatedPresetState, false, true, true);
        presetRoundTrip.setStateInformationWithOptions(repeatedPresetState.getData(),
                                                        static_cast<int>(repeatedPresetState.getSize()), false, true, true);
        compareDensePatchbayMatrix(presetRoundTrip);

        std::cout << "PASS: all " << parameters.size()
                  << " VST parameters and complete send/receive patchbay matrices round-trip through host and XML preset state.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
