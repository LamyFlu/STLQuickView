// Direct3D 11 shaders for STL Quick View, compiled by build.cmd with fxc (shader model 4.0, so
// feature level 10_0 hardware works too). The lighting reproduces the OpenGL fallback's
// fixed-function rig per pixel: ambient + key light with a soft highlight + dim fill light.

cbuffer Frame : register(b0) {
    row_major float4x4 transform;   // model -> clip
    row_major float4x4 rotation;    // model -> eye, for normals
    float4 material;                // model colour
    float4 keyDirection;            // xyz, eye space, normalised; w = key diffuse
    float4 fillDirection;           // xyz, eye space, normalised
    float4 fillColor;               // rgb fill diffuse
    float4 terms;                   // x = ambient, y = key specular, z = shininess
};

struct MeshIn { float3 position : POSITION; float3 normal : NORMAL; };
struct MeshOut { float4 position : SV_Position; float3 normal : NORMAL; };

MeshOut MeshVS(MeshIn input) {
    MeshOut output;
    output.position = mul(float4(input.position, 1), transform);
    output.normal = mul(float4(input.normal, 0), rotation).xyz;
    return output;
}

float4 MeshPS(MeshOut input) : SV_Target {
    float3 n = normalize(input.normal);
    if (n.z < 0) n = -n;   // two-sided lighting: always light the side facing the viewer
    const float3 base = material.rgb;
    const float key = dot(n, keyDirection.xyz);
    float3 color = terms.x * base;
    color += keyDirection.w * base * saturate(key);
    color += fillColor.rgb * base * saturate(dot(n, fillDirection.xyz));
    if (key > 0) {
        const float3 halfway = normalize(keyDirection.xyz + float3(0, 0, 1));
        color += terms.y * pow(saturate(dot(n, halfway)), terms.z);
    }
    return float4(saturate(color), 1);
}

struct ColorIn { float3 position : POSITION; float4 color : COLOR; };
struct ColorOut { float4 position : SV_Position; float4 color : COLOR; };

ColorOut ColorVS(ColorIn input) {
    ColorOut output;
    output.position = mul(float4(input.position, 1), transform);
    output.color = input.color;
    return output;
}

float4 ColorPS(ColorOut input) : SV_Target { return input.color; }
