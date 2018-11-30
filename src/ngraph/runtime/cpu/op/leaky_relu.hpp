//*****************************************************************************
// Copyright 2017-2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#pragma once

#include "ngraph/node.hpp"
#include "ngraph/op/op.hpp"
#include "ngraph/op/op.hpp"

namespace ngraph
{
    namespace op
    {
        /// \brief Elementwise Maximum(arg, arg * alpha) operation
        ///        alpha > 0
        ///
        class NGRAPH_API LeakyRelu : public Op
        {
        public:
            /// \brief Constructs a LeakyRelu operation.
            ///
            /// \param arg Node input to the Relu.
            LeakyRelu(std::shared_ptr<ngraph::Node> arg, float alpha);
            float get_alpha() const { return m_alpha; }
            virtual std::shared_ptr<Node>
                copy_with_new_args(const NodeVector& new_args) const override;

        private:
            float m_alpha;
        };
    }
}
