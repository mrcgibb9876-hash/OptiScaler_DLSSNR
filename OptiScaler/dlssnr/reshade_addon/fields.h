// Field tables for the DLSS 5 Neural Rendering tuning panel -- kept in sync by hand with the
// Electron app's OPTISCALER_FIELDS / FEEDER_FIELDS (OptiScalerManager's src/core/dlssnr-settings.js,
// or equivalent) so the desktop panel and both in-game routes present identical controls against the
// same keys. Not code-generated; update all three by hand together.
#pragma once

#include <cstdint>
#include <cstring>

namespace dlssnr_tune
{
	enum class field_type
	{
		boolean,
		integer,
		enumeration,
		floating
	};

	// Generic conditional, mirroring the Electron panel's showIf/disableIf: either "equals" one
	// value, or is one of a short fixed list ("in").
	struct condition
	{
		const char *key = nullptr;
		int values[4] = { 0, 0, 0, 0 };
		int value_count = 0;

		bool active() const { return key != nullptr; }
	};

	struct field
	{
		const char *key;
		field_type type;
		double default_value;
		double min = 0.0;
		double max = 0.0;
		double step = 0.01;
		const char *label;
		const char *group;
		const char *help = nullptr;
		bool percent = false;
		bool advanced = false; // excluded from the UI entirely
		condition show_if{};
		condition disable_if{};
		const char *const *options = nullptr;
		int option_count = 0;
	};

	struct field_table
	{
		const field *fields;
		int count;
		const char *section;
	};

	inline const char *const style_options[] = { "Default (standard)", "Natural", "Cinematic" };
	inline const char *const preset_options[] = { "Default", "Model A", "Model B", "Model C" };
	inline const char *const depth_options[] = { "Follow the game", "Force normal", "Force inverted" };
	inline const char *const compare_options[] = { "Off", "Side by side", "Wipe" };
	inline const char *const debug_view_options[] = { "Off", "Proxy (what the model sees)", "Model output (raw)", "Difference (amplified)" };
	inline const char *const reversible_options[] = { "Off (soft knee)", "Neutwo proxy + composed", "Neutwo proxy + replace", "Hybrid proxy + composed", "Hybrid proxy + replace" };
	inline const char *const white_point_source_options[] = { "Paper white only", "The game's own exposure", "A buffer the scan found" };

	// feeder/RenoDX route: [RenoDX.DLSS5], read/written through ReShade's own config API
	// (get_config_value/set_config_value) -- the same store RenoDX itself reads.
	inline const field feeder_fields[] = {
		{ "NRStyle", field_type::enumeration, 0, 0, 0, 0, "Style", "main", nullptr, false, false, {}, {}, style_options, 3 },
		{ "NRIntensity", field_type::floating, 1.0, 0.0, 2.0, 0.01, "Intensity", "main" },
		{ "NRUICorrection", field_type::boolean, 1, 0, 0, 0, "UI correction", "main" },
		{ "NRToggleKey", field_type::integer, 0, 0, 0, 0, "Toggle key (virtual-key code, 0 = unbound)", "main",
			"RenoDX keeps its on/off state in memory only, not in this config -- nothing here turns NR on or off directly. Set a key here and press it in-game, or use ReShade's own overlay -> RenoDX tab -> \"Enable DLSS Neural Rendering\"." },

		{ "NRLocalStructure", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Structure intensity", "global" },
		{ "NRLocalTone", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Tone intensity", "global" },

		{ "NRAutoMask", field_type::boolean, 1, 0, 0, 0, "Model automask", "automask" },
		{ "NRSkinStructure", field_type::floating, -1.0, -1.0, 1.0, 0.01, "Skin structure intensity", "automask",
			"-1 follows Structure intensity above; 0 and above set the masked region independently.", false, false, {}, condition{ "NRAutoMask", {1,0,0,0}, 1 } },

		{ "NRPreset", field_type::enumeration, 0, 0, 0, 0, "Model", "models", nullptr, false, false, {}, {}, preset_options, 4 },

		{ "NRTransferStrength", field_type::floating, 1.0, 0.0, 2.0, 0.01, "Detail strength", "transfer" },
		{ "NRColorStrength", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Colour strength", "transfer" },

		{ "NRPaperWhiteScale", field_type::floating, 1.0, 0.25, 4.0, 0.01, "Paper white", "colour" },
		{ "NRGlobalTone", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Global tone", "colour", "Guessed range -- no OptiScaler analog to confirm against." },
		{ "NRDiffuseWhiteNits", field_type::floating, 203.0, 80.0, 1000.0, 1.0, "Diffuse white (nits)", "colour", "Guessed range -- 203 nits is the ITU-R BT.2100 reference white, used as a placeholder default." },

		{ "NRDepthMode", field_type::enumeration, 0, 0, 0, 0, "Depth", "guide", nullptr, false, false, {}, {}, depth_options, 3 },

		{ "NRScreenshotKey", field_type::integer, 0, 0, 0, 0, "Screenshot key (virtual-key code, 0 = unbound)", "inspect" },

		{ "NRMVecScaleX", field_type::floating, 1.0, 0.1, 4.0, 0.01, "Motion vector scale X", "experimental", "Guessed range -- no OptiScaler analog to confirm against." },
		{ "NRMVecScaleY", field_type::floating, 1.0, 0.1, 4.0, 0.01, "Motion vector scale Y", "experimental", "Guessed range -- no OptiScaler analog to confirm against." },
	};
	inline constexpr int feeder_field_count = sizeof(feeder_fields) / sizeof(feeder_fields[0]);
	inline constexpr field_table feeder_table = { feeder_fields, feeder_field_count, "RenoDX.DLSS5" };

	// OptiScaler route: [DlssNr] in OptiScaler.ini -- read/written through OptiScaler's own live
	// control ABI (../DlssNr_Api.h), not raw ini text, so a change here reaches the running pass
	// exactly when the desktop app's own "Enabled" toggle would.
	inline const field optiscaler_fields[] = {
		{ "Enabled", field_type::boolean, 0, 0, 0, 0, "DLSS-NR enabled", "main",
			"Synthesises detail in the upscaler's output, before frame generation sees it.\n\nNeeds nvngx_dlssnr.dll beside OptiScaler, plus the forwarder that ships with it." },
		{ "ToggleKey", field_type::integer, 0, 0, 0, 0, "Toggle key (virtual-key code, 0 = unbound)", "main" },

		{ "LocalStructure", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Structure intensity", "global" },
		{ "LocalTone", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Tone intensity", "global" },

		{ "AutoMask", field_type::boolean, 1, 0, 0, 0, "Model automask", "automask" },
		{ "SkinStructure", field_type::floating, -1.0, -1.0, 1.0, 0.01, "Skin structure intensity", "automask",
			"-1 follows Structure intensity above; 0 and above set the masked region independently.", false, false, {}, condition{ "AutoMask", {1,0,0,0}, 1 } },

		{ "Preset", field_type::enumeration, 0, 0, 0, 0, "Model", "models", nullptr, false, false, {}, {}, preset_options, 4 },
		{ "Style", field_type::enumeration, 0, 0, 0, 0, "Style", "models", nullptr, false, false, {}, {}, style_options, 3 },
		{ "Intensity", field_type::floating, 1.0, 0.0, 2.0, 0.01, "Intensity", "models" },

		{ "WorkingScale", field_type::floating, 1.0, 0.25, 1.0, 0.01, "Model resolution", "cost",
			"Cost falls with the square of this; the frame itself is never reduced, only the model's own contribution.", true },

		{ "TransferStrength", field_type::floating, 1.0, 0.0, 2.0, 0.01, "Detail strength", "transfer",
			"How far the frame moves toward the model's picture. 0 gives back exactly what the upscaler produced, 1 is the model's picture, above 1 carries on past it." },
		{ "ColourStrength", field_type::floating, 1.0, 0.0, 1.0, 0.01, "Colour strength", "transfer",
			"Whether the model's colour arrives with its light. 0 keeps the game's own hue exactly. Above 1 over-saturates, keeping hue but growing more vivid." },

		{ "WhitePointScale", field_type::floating, 1.0, 0.25, 4.0, 0.01, "Paper white", "colour",
			"What the frame is divided by before the model sees it. Raise it until the picture stops improving -- past that point it does not plateau, it gets worse the other way." },
		{ "MaxRatio", field_type::floating, 2.0, 1.0, 8.0, 0.1, "Highlight guard", "colour",
			"The most the pass may move any pixel, as a multiple of what it already was. Raise it only if bright areas look clipped." },
		{ "ReversibleMode", field_type::enumeration, 0, 0, 0, 0, "Reversible proxy", "colour",
			"What the model is shown, and how its answer comes back. Hybrid composed is the one to use: identity in the midtones, unclipped roll only in the highlights. Off is the original behaviour.",
			false, false, {}, {}, reversible_options, 5 },

		{ "DepthConvention", field_type::enumeration, 0, 0, 0, 0, "Depth", "guide", nullptr, false, false, {}, {}, depth_options, 3 },
		{ "UICorrection", field_type::boolean, 1, 0, 0, 0, "UI correction", "guide" },

		{ "AutoCapture", field_type::boolean, 1, 0, 0, 0, "Auto-capture once per session", "inspect" },
		{ "ApplyModel", field_type::boolean, 1, 0, 0, 0, "Apply the model", "inspect",
			"Whether the model's edit is applied. Off shows the clean upscaler frame while the pass keeps running, so with Hold frame you can freeze one frame and toggle this to see it with and without." },
		{ "HoldFrame", field_type::boolean, 0, 0, 0, 0, "Hold frame", "inspect",
			"Freezes the frame the model works on, so a setting change re-renders it in place. The only clean way to A/B settings, since a moving scene confounds everything else." },
		{ "Compare", field_type::enumeration, 0, 0, 0, 0, "Compare", "inspect", nullptr, false, false, {}, {}, compare_options, 3 },
		{ "CompareSwap", field_type::boolean, 0, 0, 0, 0, "Swap sides", "inspect", nullptr, false, false, condition{ "Compare", {1,2,0,0}, 2 } },
		{ "CompareZoom", field_type::floating, 1.0, 1.0, 2.0, 0.01, "Zoom", "inspect", nullptr, false, false, condition{ "Compare", {1,0,0,0}, 1 } },
		{ "CompareSplit", field_type::floating, 0.5, 0.0, 1.0, 0.01, "Split", "inspect", nullptr, false, false, condition{ "Compare", {2,0,0,0}, 1 } },
		{ "DebugView", field_type::enumeration, 0, 0, 0, 0, "Debug view", "inspect", nullptr, false, false, {}, {}, debug_view_options, 4 },

		{ "ProxyProbe", field_type::boolean, 0, 0, 0, 0, "Probe the driver", "experimental",
			"Unproven -- asks the driver's own nvngx.dll whether it already knows the model." },
		{ "UseProxy", field_type::boolean, 0, 0, 0, 0, "Run through the driver", "experimental",
			"Unproven -- drives the model through the driver's own nvngx.dll instead of the forwarder." },

		{ "ForceSdkVersion", field_type::integer, 0, 0, 0, 0, "Force SDK version (0 = auto)", "experimental", nullptr, false, true },
	};
	inline constexpr int optiscaler_field_count = sizeof(optiscaler_fields) / sizeof(optiscaler_fields[0]);
	inline constexpr field_table optiscaler_table = { optiscaler_fields, optiscaler_field_count, "DlssNr" };

	inline const char *group_title(const char *group)
	{
		if (strcmp(group, "main") == 0) return "General";
		if (strcmp(group, "global") == 0) return "Global Controls";
		if (strcmp(group, "automask") == 0) return "Model Automask";
		if (strcmp(group, "models") == 0) return "Models";
		if (strcmp(group, "cost") == 0) return "Cost";
		if (strcmp(group, "transfer") == 0) return "How much of it lands";
		if (strcmp(group, "colour") == 0) return "Colour";
		if (strcmp(group, "guide") == 0) return "Guide";
		if (strcmp(group, "inspect") == 0) return "Inspect";
		if (strcmp(group, "experimental") == 0) return "Experimental";
		return group;
	}
}
