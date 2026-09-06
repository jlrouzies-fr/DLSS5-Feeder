// Source-guided Color Refinement B++ -- local AI-assisted prototype.
// Required order: SourceGuided_Save -> DLSS5_Feed -> SourceGuided_Apply.
// No closed NR changes, extra NGX evaluation, fixed UI colours or screen boxes.
// Oklab coefficients: https://bottosson.github.io/posts/oklab/ (public domain).
// FX resource/sampler/pass semantics:
// https://github.com/crosire/reshade-shaders/blob/slim/REFERENCE.md
// Tested compiler target: ReShade 6.8.0, commit 18deaa52de0c425a78b329e9cb3c497281cd00ec.
// Compile-off removes ALL resources and passes. Runtime bypass retains their cost.
#ifndef SOURCE_GUIDED_COLOR
#define SOURCE_GUIDED_COLOR 0
#endif

#if SOURCE_GUIDED_COLOR == 1 && __RENDERER__ >= 0xb000 && __RENDERER__ < 0xc000 && BUFFER_COLOR_SPACE == 1 && BUFFER_COLOR_BIT_DEPTH == 8

uniform uint SGCR_Frame < source = "framecount"; >;
uniform bool SGCR_Bypass < source = "key"; keycode = 118; mode = "toggle"; >;
uniform bool SGCR_Enabled <
    ui_label = "Enable source-guided colours";
    ui_tooltip = "SDR sRGB prototype. F7 temporarily bypasses refinement only; F6 remains NR. Both techniques must surround DLSS5_Feed.";
> = false;
uniform float SGCR_Strength <
    ui_type = "slider"; ui_label = "Colour separation";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip = "0: neural output. 0.90: selected B++. Can also attenuate useful broad NR colour changes.";
> = 0.9;
uniform float SGCR_Tone <
    ui_type = "slider"; ui_label = "Tone and contrast correction";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_tooltip = "Affects the lightness base of every colour, not just white text.";
> = 1.0;
uniform float SGCR_Detail <
    ui_type = "slider"; ui_label = "Neural residual detail";
    ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
    ui_tooltip = "1.00 preserves the neural residual. Larger values can amplify noise and halos.";
> = 1.0;
uniform int SGCR_View <
    ui_type = "combo"; ui_label = "Comparison view";
    ui_items = "Processed\0Source before Feeder\0Neural / post Feeder\0";
> = 0;

texture SourceGuided_Color : COLOR;
texture SourceGuided_SourceLab { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA32F; };
texture SourceGuided_NeuralLab { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA32F; };
sampler SGCR_Color {
    Texture = SourceGuided_Color; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point; SRGBTexture = false;
};
sampler SGCR_Source {
    Texture = SourceGuided_SourceLab; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point; SRGBTexture = false;
};
sampler SGCR_Neural {
    Texture = SourceGuided_NeuralLab; AddressU = Clamp; AddressV = Clamp;
    MinFilter = Point; MagFilter = Point; MipFilter = Point; SRGBTexture = false;
};

void SGCR_VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 SGCR_ToLab(float3 rgb) {
    // Encoded sRGB is converted exactly once; samplers and writes are non-sRGB.
    float3 hi = pow((saturate(rgb) + 0.055) / 1.055, 2.4);
    float3 lin = float3(rgb.r <= 0.04045 ? rgb.r/12.92 : hi.r,
                        rgb.g <= 0.04045 ? rgb.g/12.92 : hi.g,
                        rgb.b <= 0.04045 ? rgb.b/12.92 : hi.b);
    float3 lms = float3(dot(lin, float3(.4122214708, .5363325363, .0514459929)),
                        dot(lin, float3(.2119034982, .6806995451, .1073969566)),
                        dot(lin, float3(.0883024619, .2817188376, .6299787005)));
    lms = pow(max(lms, 0.0), 1.0/3.0);
    return float3(dot(lms, float3(.2104542553, .7936177850, -.0040720468)),
                  dot(lms, float3(1.9779984951, -2.4285922050, .4505937099)),
                  dot(lms, float3(.0259040371, .7827717662, -.8086757660)));
}

float3 SGCR_ToLinear(float3 lab) {
    // Inverses of the exact matrices used by the existing CPU reference.
    float3 lms = float3(dot(lab, float3(.999999998450520, .396337792173768, .215803758060759)),
                        dot(lab, float3(1.000000008881761, -.105561342323656, -.063854174771706)),
                        dot(lab, float3(1.000000054672411, -.089484182094966, -1.291485537864092)));
    lms = lms*lms*lms;
    return float3(dot(lms, float3(4.076741661347994, -3.307711590408193, .230969928729428)),
                  dot(lms, float3(-1.268438004092176, 2.609757400663371, -.341319396310220)),
                  dot(lms, float3(-.004196086541837, -.703418614459450, 1.707614700930945)));
}

float3 SGCR_Encode(float3 lin) {
    lin = saturate(lin);
    float3 hi = 1.055 * pow(lin, 1.0/2.4) - .055;
    return float3(lin.r <= .0031308 ? lin.r*12.92 : hi.r,
                  lin.g <= .0031308 ? lin.g*12.92 : hi.g,
                  lin.b <= .0031308 ? lin.b*12.92 : hi.b);
}

bool SGCR_Fits(float3 lin) {
    return all(lin >= -1e-9) && all(lin <= 1.0 + 1e-9);
}

float3 SGCR_Compress(float3 lab) {
    lab.x = saturate(lab.x);
    float3 lin = SGCR_ToLinear(lab);
    if (SGCR_Fits(lin)) return SGCR_Encode(lin);
    float low = 0.0, high = 1.0;
    [loop] for (int i = 0; i < 16; ++i) {
        float mid = (low + high) * .5;
        if (SGCR_Fits(SGCR_ToLinear(float3(lab.x, lab.yz * mid)))) low = mid;
        else high = mid;
    }
    return SGCR_Encode(SGCR_ToLinear(float3(lab.x, lab.yz * low)));
}

float4 SGCR_SavePS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    // A zero/uninitialised target is invalid. Exact float32 integer stamps avoid
    // modulo wrap accepting a stale source. Beyond this range, fail closed.
    float stamp = SGCR_Frame <= 16777214u ? float(SGCR_Frame + 1u) : 0.0;
    return float4(SGCR_ToLab(tex2D(SGCR_Color, uv).rgb), stamp);
}

float4 SGCR_NeuralPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float4 neural = tex2D(SGCR_Color, uv);
    return float4(SGCR_ToLab(neural.rgb), neural.a);
}

float4 SGCR_ApplyPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float4 originalNeural = tex2D(SGCR_Color, uv);
    if (!SGCR_Enabled || SGCR_Bypass || SGCR_View == 2 ||
        (SGCR_Strength <= 0.0 && SGCR_View != 1)) return originalNeural;
    float4 source = tex2D(SGCR_Source, uv);
    if (SGCR_Frame > 16777214u || source.a != float(SGCR_Frame + 1u)) return originalNeural;
    if (SGCR_View == 1) return float4(SGCR_Encode(SGCR_ToLinear(source.rgb)), originalNeural.a);

    float3 neural = tex2D(SGCR_Neural, uv).rgb;
    float3 sourceSum = 0.0, neuralSum = 0.0;
    float weightSum = 0.0;
    const float2 pixel = float2(1.0 / BUFFER_WIDTH, 1.0 / BUFFER_HEIGHT);
    // Exact radius-4 joint bilateral B++ reference, not separable approximation.
    [loop] for (int dy = -4; dy <= 4; ++dy) {
        [loop] for (int dx = -4; dx <= 4; ++dx) {
            float2 at = uv + float2(dx, dy) * pixel;
            float3 sampleSource = tex2Dlod(SGCR_Source, float4(at, 0, 0)).rgb;
            float3 delta = sampleSource - source.rgb;
            float distance = delta.x*delta.x / .01 + dot(delta.yz, delta.yz) / .003025;
            distance += float(dx*dx + dy*dy) / 6.76; // (4 * .65)^2
            float weight = exp(-.5 * distance);
            sourceSum += sampleSource * weight;
            neuralSum += tex2Dlod(SGCR_Neural, float4(at, 0, 0)).rgb * weight;
            weightSum += weight;
        }
    }
    float3 sourceBase = sourceSum / weightSum;
    float3 neuralBase = neuralSum / weightSum;
    float strength = saturate(SGCR_Strength);
    float3 correction = sourceBase - neuralBase;
    correction.x *= saturate(SGCR_Tone);
    float3 outputLab = neural + strength * correction;
    outputLab += strength * (clamp(SGCR_Detail, 0.0, 2.0) - 1.0) * (neural - neuralBase);
    return float4(SGCR_Compress(outputLab), originalNeural.a);
}

technique SourceGuided_Save < ui_label = "Source colours: BEFORE Feeder"; > {
    pass SaveOriginalColourAreas {
        VertexShader = SGCR_VS; PixelShader = SGCR_SavePS;
        RenderTarget = SourceGuided_SourceLab;
        SRGBWriteEnable = false;
    }
}
technique SourceGuided_Apply < ui_label = "Source-guided colours: AFTER Feeder"; > {
    pass ReadNeuralColourAreas {
        VertexShader = SGCR_VS; PixelShader = SGCR_NeuralPS;
        RenderTarget = SourceGuided_NeuralLab;
        SRGBWriteEnable = false;
    }
    pass RefineColourAreasAndDetails {
        VertexShader = SGCR_VS; PixelShader = SGCR_ApplyPS;
        SRGBWriteEnable = false;
    }
}
#endif
