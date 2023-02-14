from typing import Dict, Optional, Sequence, Tuple

import torch.distributed as dist
import torch.nn as nn
from torch.distributed._spmd.distribute import distribute, Schema
from torch.distributed._spmd.distributed_graph import DistributedGraph
from torch.distributed._tensor import Placement, Replicate


class SPMD(nn.Module):
    def __init__(
        self,
        module: nn.Module,
        schema: Schema,
        input_schemas: Sequence[Placement] = tuple(),
        expand_first_iter: bool = False,
    ) -> None:
        """
        Given a non-distributed nn.Module, distribute the module and apply
        optimizations over the distributed module (fx.GraphModule).

        Args:
            module (nn.Module): The target module.
            schema (Schema): The distributed schema.
            input_schemas (Sequence[Placement]): The schemas of the inputs.
            expand_first_iter (bool): If true, SPMD will call the forward
               and backward passes to eagerly get the graphs. This can be
               problematic since SPMD currently assumes a simple out of
               the tensor and performs ``sum()`` to get the loss.
        """
        super().__init__()
        assert schema.placements == [
            Replicate()
        ], "SPMD only support Replicate() parameters for now"

        # TODO: Fix model initialization with coalescing.
        # This needs to happen post model transformation.
        # Consider an explicit model init API.
        for p in module.parameters():
            dist.broadcast(p, src=0)

        self._param_schema = schema
        self._input_schemas = input_schemas
        self._compiled_m: Optional[nn.Module] = None
        self._dist_graph = DistributedGraph(orig_module=module)
        self._expand_first_iter = expand_first_iter

    def forward(self, *args: Tuple[object], **kwargs: Dict[str, object]) -> object:
        if self._compiled_m is None:
            self._compiled_m = distribute(
                self._dist_graph,
                self._param_schema,
                self._input_schemas,
                self._expand_first_iter,
                *args,
                **kwargs,
            )

        assert self._compiled_m is not None
        return self._compiled_m(*args, **kwargs)
