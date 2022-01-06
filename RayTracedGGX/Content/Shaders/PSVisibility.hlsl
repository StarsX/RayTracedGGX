//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define PRIMITIVE_BITS 24

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	uint g_instanceIdx;
};

//--------------------------------------------------------------------------------------
// Base visiblity-buffer pass
//--------------------------------------------------------------------------------------
uint main(uint primitiveId : SV_PrimitiveID, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
	//const uint frontFaceBit = isFrontFace ? 1 : 0;

	//return ((frontFaceBit << 31) | (g_instanceIdx << PRIMITIVE_BITS) | primitiveId) + 1;
	return ((g_instanceIdx << PRIMITIVE_BITS) | primitiveId) + 1;
}
