#include "rt-neural-generic.h"

/**********************************************************************************************************************************************************/

static const LV2_Descriptor Descriptor = {
    PLUGIN_URI,
    RtNeuralGeneric::instantiate,
    RtNeuralGeneric::connect_port,
    RtNeuralGeneric::activate,
    RtNeuralGeneric::run,
    RtNeuralGeneric::deactivate,
    RtNeuralGeneric::cleanup,
    RtNeuralGeneric::extension_data
};

/**********************************************************************************************************************************************************/

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    if (index == 0) return &Descriptor;
    else return NULL;
}

/**********************************************************************************************************************************************************/

// Apply a ramp to the value to avoid zypper noise
float RtNeuralGeneric::rampValue(float start, float end, uint32_t n_samples, uint32_t index) {
    return (start + ((end - start)/n_samples) * index);
}

/**********************************************************************************************************************************************************/

// Apply a gain ramp to a buffer
void RtNeuralGeneric::applyGainRamp(float *buffer, float start, float end, uint32_t n_samples) {
    static uint32_t i;
    for(i=0; i<n_samples; i++) {
        buffer[i] *= rampValue(start, end, n_samples, i);
    }
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::loadModel(LV2_Handle instance, const char *bundle_path, const char *fileName)
{
    RtNeuralGeneric *plugin;
    plugin = (RtNeuralGeneric *) instance;

    std::string filePath;

    filePath.append(bundle_path);
    filePath.append(fileName);

    std::cout << "Loading json file: " << filePath << std::endl;

    try {
        std::ifstream jsonStream(filePath, std::ifstream::binary);
        std::ifstream jsonStream2(filePath, std::ifstream::binary);
        nlohmann::json modelData;
        jsonStream2 >> modelData;
        plugin->model.parseJson(jsonStream, true);

        plugin->n_layers = modelData["layers"].size();
        plugin->input_size = modelData["in_shape"].back().get<int>();

        if (modelData["in_skip"].is_number()) {
            plugin->input_skip = modelData["in_skip"].get<int>();
            if (plugin->input_skip > 1)
                throw std::invalid_argument("Values for in_skip > 1 are not supported");
        }
        else {
            plugin->input_skip = 0;
        }

        plugin->type = modelData["layers"][plugin->n_layers-1]["type"];
        plugin->hidden_size = modelData["layers"][plugin->n_layers-1]["shape"].back().get<int>();

        // If we are good: let's say so
        plugin->model_loaded = 1;

        std::cout << "Successfully loaded json file: " << filePath << std::endl;
    }
    catch (const std::exception& e) {
        std::cout << std::endl << "Unable to load json file: " << filePath << std::endl;
        std::cout << e.what() << std::endl;

        // If we are not good: let's say no
        plugin->model_loaded = 0;
    }
}

/**********************************************************************************************************************************************************/

LV2_Handle RtNeuralGeneric::instantiate(const LV2_Descriptor* descriptor, double samplerate, const char* bundle_path, const LV2_Feature* const* features)
{
    RtNeuralGeneric *plugin = new RtNeuralGeneric();

    // Load model json file
    plugin->loadModel((LV2_Handle)plugin, bundle_path, JSON_MODEL_FILE_NAME);

    // Before running inference, it is recommended to "reset" the state
    // of your model (if the model has state).
    plugin->model.reset();

    // Pre-buffer to avoid "clicks" during initialization
    float in[2048] = { };
    for(int i=0; i<2048; i++) {
        plugin->model.forward(in + i);
    }

    // Setup fixed frequency dc blocker filter (high pass)
    plugin->dc_blocker_fp.nType        = lsp::dspu::FLT_BT_RLC_HIPASS;
    plugin->dc_blocker_fp.fFreq        = 35.0f;
    plugin->dc_blocker_fp.fGain        = 1;
    plugin->dc_blocker_fp.nSlope       = 1;
    plugin->dc_blocker_fp.fQuality     = 0.0f;

    plugin->dc_blocker_f.init(NULL);   // Use own internal filter bank
    plugin->dc_blocker_f.update(samplerate, &(plugin->dc_blocker_fp)); // Apply filter settings

    lsp::dsp::init();

    plugin->bypass_old = 0;

    return (LV2_Handle)plugin;
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::activate(LV2_Handle instance)
{
    // TODO: include the activate function code here
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::deactivate(LV2_Handle instance)
{
    // TODO: include the deactivate function code here
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::connect_port(LV2_Handle instance, uint32_t port, void *data)
{
    RtNeuralGeneric *plugin;
    plugin = (RtNeuralGeneric *) instance;

    switch (port)
    {
        case IN:
            plugin->in = (float*) data;
            break;
        case OUT_1:
            plugin->out_1 = (float*) data;
            break;
        case PARAM1:
            plugin->param1 = (float*) data;
            break;
        case PARAM2:
            plugin->param2 = (float*) data;
            break;
        case MASTER:
            plugin->master = (float*) data;
            break;
        case BYPASS:
            plugin->bypass = (int*) data;
            break;
    }
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::run(LV2_Handle instance, uint32_t n_samples)
{
    RtNeuralGeneric *plugin;
    plugin = (RtNeuralGeneric *) instance;

    lsp::dsp::context_t ctx;

    float param1 = *plugin->param1;
    float param2 = *plugin->param2;
    int bypass = *plugin->bypass; // NOTE: since float 1.0 is sent instead of (int 32bit) 1, then we have 1065353216 as 1
    float master, master_old, tmp;
    uint32_t i;

    master = *plugin->master;
    master_old = plugin->master_old;
    plugin->master_old = master;

    if (bypass != plugin->bypass_old) {
        std::cout << "Bypass status changed to: " << bypass << std::endl;
        plugin->bypass_old = bypass;
    }

    if (bypass == 0) {
        if (plugin->model_loaded == 1) {
            // Process model based on input_size (snapshot model or conditioned model)
            switch(plugin->input_size) {
                case 1:
                    for(i=0; i<n_samples; i++) {
                        plugin->out_1[i] = plugin->model.forward(plugin->in + i) + (plugin->in[i] * plugin->input_skip);
                    }
                    break;
                case 2:
                    for(i=0; i<n_samples; i++) {
                        plugin->inArray1[0] = plugin->in[i];
                        plugin->inArray1[1] = param1;
                        plugin->out_1[i] = plugin->model.forward(plugin->inArray1) + (plugin->in[i] * plugin->input_skip);
                    }
                    break;
                case 3:
                    for(i=0; i<n_samples; i++) {
                        plugin->inArray2[0] = plugin->in[i];
                        plugin->inArray2[1] = param1;
                        plugin->inArray2[2] = param2;
                        plugin->out_1[i] = plugin->model.forward(plugin->inArray2) + (plugin->in[i] * plugin->input_skip);
                    }
                    break;
                default:
                    break;
            }

            lsp::dsp::start(&plugin->ctx);
            plugin->dc_blocker_f.process(plugin->out_1, plugin->out_1, n_samples);
            lsp::dsp::finish(&plugin->ctx);
            applyGainRamp(plugin->out_1, master_old, master, n_samples); // Master volume
        }
    }
    else
    {
        std::copy(plugin->in, plugin->in + n_samples, plugin->out_1); // Passthrough
    }
}

/**********************************************************************************************************************************************************/

void RtNeuralGeneric::cleanup(LV2_Handle instance)
{
    delete ((RtNeuralGeneric *) instance);
}

/**********************************************************************************************************************************************************/

const void* RtNeuralGeneric::extension_data(const char* uri)
{
    return NULL;
}
