import { initialState } from "./store";

export const findNode = (state = initialState, action) => {
  switch (action.type) {
    case "setFindNode":
      return action.payload;

    default:
      return state;
  }
};

export const setFindNode = (node) => ({
  type: "setFindNode",
  payload: node,
});
