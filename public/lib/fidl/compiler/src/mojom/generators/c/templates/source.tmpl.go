// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const GenerateSourceFile = `
{{- define "GenerateSourceFile" -}}
// This file was auto-generated by the C bindings generator.

#include "{{.HeaderFile}}"

#include <mojo/bindings/internal/type_descriptor.h>
#include <mojo/system/handle.h>
#include <stdbool.h>

// Imports.
{{range $import := .Imports -}}
#include "{{$import}}"
{{end}}

// Type tables definitions for structs, arrays and unions.
{{template "GenerateTypeTableDefinitions" .TypeTable}}

// Definitions for constants.
// Top level constants:
{{range $const := .Constants -}}
const {{$const.Type}} {{$const.Name}} = {{$const.Value}};
{{end -}}

// Struct definitions.
{{range $struct := .Structs -}}
{{template "GenerateStructDefinitions" $struct}}
{{end -}}

// Interface definitions.
{{range $interface := .Interfaces -}}
{{template "GenerateInterfaceDefinitions" $interface}}
{{end -}}

{{end}}
`
