# Dependencies and provenance

- CommonLibSSE-NG v3.7.0: https://github.com/CharmedBaryon/CommonLibSSE-NG — MIT.
  Built from its tagged source by CMake. Its complete license is included in the
  artifact at docs/DawnwalkerCombat/licenses/CommonLibSSE-NG.txt.
- fmt and spdlog: installed from Microsoft vcpkg tag 2023.10.19; their copyright
  notices are copied from the installed packages into the artifact.
- rapidcsv: a CommonLib build dependency from the same vcpkg tag.
- vcpkg: https://github.com/microsoft/vcpkg — build tooling, not shipped.
- Behavior Data Injector: external runtime requirement, not bundled or linked.
  Our JSON follows the author's documented config format:
  https://github.com/max-su-2019/BehaviorDataInjector/blob/master/doc/How%20to%20create%20BDI%20config%20files.md
- OAR: external runtime consumer; our optional configs use its CompareValues
  format and do not include OAR source or binary code.
- DMK/Direction-Movement was suggested as an API reference only. Its source was
  not copied, adapted or used to implement this milestone.
