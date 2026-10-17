// The Greyscale Audio logo for the screen: a vacuum tube whose grid is a
// small neural network, in amber. Same drawing as
// resources/logo/greyscale-audio-logo.svg.

#pragma once

namespace t3k {

constexpr const char* kLogoSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 120" width="100" height="120">
  <g fill="none" stroke="#C8501E" stroke-linecap="round" stroke-linejoin="round">
    <path d="M32 92V42C32 22 68 22 68 42V92" stroke-width="4"/>
    <path d="M26 92H74" stroke-width="4"/>
    <path d="M40 92V104M50 92V106M60 92V104" stroke-width="4"/>
    <path d="M42 48L38 62L42 78M42 48L50 62L42 78M42 48L62 62L42 78M58 48L38 62L58 78M58 48L50 62L58 78M58 48L62 62L58 78" stroke-width="2"/>
  </g>
  <g fill="#C8501E">
    <circle cx="42" cy="48" r="3.6"/>
    <circle cx="58" cy="48" r="3.6"/>
    <circle cx="38" cy="62" r="3.6"/>
    <circle cx="50" cy="62" r="3.6"/>
    <circle cx="62" cy="62" r="3.6"/>
    <circle cx="42" cy="78" r="3.6"/>
    <circle cx="58" cy="78" r="3.6"/>
  </g>
</svg>)svg";

}  // namespace t3k
