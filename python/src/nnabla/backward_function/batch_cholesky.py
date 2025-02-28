# Copyright 2022 Sony Group Corporation.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# *WARNING*
# THIS FILE IS AUTO-GENERATED BY CODE GENERATOR.
# 1. IMPLEMENT BACKWARD WRT INPUTS OF THE CORRESPONDING FUNCTION
# 2. IMPLEMENT BACKWARD_FUNCTION_CLASS IF NECESSARY (see e.g., affine.py)
# 3. UPDATE THE MAPPING IF NECESSARY (see function_backward_functions.py.tmpl)


import nnabla.functions as F


def batch_cholesky_backward(inputs):
    """
    Args:
      inputs (list of nn.Variable): Incomming grads/inputs to/of the forward function.
      kwargs (dict of arguments): Dictionary of the corresponding function arguments.

    Return:
      list of Variable: Return the gradients wrt inputs of the corresponding function.
    """
    dy = inputs[0]
    x0 = inputs[1]
    raise NotImplementedError("batch_cholesky_backward is not implemented.")
