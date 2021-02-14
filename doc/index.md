# heyoka.py

> The [heyókȟa](https://en.wikipedia.org/wiki/Heyoka) \[\...\] is a kind
> of sacred clown in the culture of the Sioux (Lakota and Dakota people)
> of the Great Plains of North America. The heyoka is a contrarian,
> jester, and satirist, who speaks, moves and reacts in an opposite
> fashion to the people around them.

heyoka is a Python library for the integration of ordinary differential
equations (ODEs) via Taylor\'s method. Notable features include:

- support for both double-precision and extended-precision
  floating-point types (80-bit and 128-bit),
- the ability to maintain machine precision accuracy over tens of
  billions of timesteps,
- batch mode integration to harness the power of modern
  [SIMD](https://en.wikipedia.org/wiki/SIMD) instruction sets,
- a high-performance implementation of Taylor\'s method based on
  automatic differentiation techniques and aggressive just-in-time
  compilation via [LLVM](https://llvm.org/).

heyoka.py is based on the [heyoka C++ library](https://bluescarni.github.io/heyoka/).

## Contents

```{toctree}
:maxdepth: 1
:caption: Basic tutorials

tut_taylor_method
notebooks/The expression system.ipynb
notebooks/ODEs with parameters.ipynb
```

```{toctree}
:maxdepth: 1
:caption: Advanced tutorials

notebooks/Long term stability of Trappist-1
```

## Authors

- Francesco Biscani (Max Planck Institute for Astronomy)
- Dario Izzo (European Space Agency)

## License

heyoka is released under the [MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/FAQ/) license.